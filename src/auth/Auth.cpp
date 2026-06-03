/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "auth/Auth.h"
#include "auth/CborReader.h"

#include <catapult/crypto.hpp>
#include <catapult/cwt.hpp>
#include <catapult/error.hpp>
#include <catapult/moqt_claims.hpp>
#include <folly/Conv.h>
#include <folly/Expected.h>
#include <folly/Range.h>
#include <folly/logging/xlog.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

using namespace moxygen;

namespace openmoq::moqx::auth {
namespace {

std::string canonicalNamespace(const TrackNamespace& ns) {
  std::string out;
  for (const auto& field : ns.trackNamespace) {
    out.push_back(static_cast<char>((field.size() >> 24) & 0xff));
    out.push_back(static_cast<char>((field.size() >> 16) & 0xff));
    out.push_back(static_cast<char>((field.size() >> 8) & 0xff));
    out.push_back(static_cast<char>(field.size() & 0xff));
    out.append(field);
  }
  return out;
}

// Derive a 256-bit HMAC key from the configured secret using HKDF-SHA256
// (RFC 5869) rather than a bare hash. The fixed, non-secret salt and info
// strings provide domain separation so the same configured secret can't yield
// identical key material if reused for another purpose. (HKDF does not add
// entropy: a low-entropy secret is still weak -- operators should use a long,
// random secret.)
constexpr std::string_view kHkdfSalt = "moqx-catapult-v1";
constexpr std::string_view kHkdfInfo = "moqx-catapult-hmac-token-verify";

std::vector<uint8_t> deriveHmacKey(std::string_view secret) {
  std::vector<uint8_t> key(32); // 256-bit output for HMAC-SHA256

  auto* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
  if (!ctx) {
    throw std::runtime_error("EVP_PKEY_CTX_new_id(HKDF) failed");
  }
  auto guard = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>(ctx, EVP_PKEY_CTX_free);

  size_t keyLen = key.size();
  if (EVP_PKEY_derive_init(ctx) <= 0 || EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha256()) <= 0 ||
      EVP_PKEY_CTX_set1_hkdf_salt(
          ctx,
          reinterpret_cast<const unsigned char*>(kHkdfSalt.data()),
          static_cast<int>(kHkdfSalt.size())
      ) <= 0 ||
      EVP_PKEY_CTX_set1_hkdf_key(
          ctx,
          reinterpret_cast<const unsigned char*>(secret.data()),
          static_cast<int>(secret.size())
      ) <= 0 ||
      EVP_PKEY_CTX_add1_hkdf_info(
          ctx,
          reinterpret_cast<const unsigned char*>(kHkdfInfo.data()),
          static_cast<int>(kHkdfInfo.size())
      ) <= 0 ||
      EVP_PKEY_derive(ctx, key.data(), &keyLen) <= 0) {
    throw std::runtime_error("HKDF key derivation failed");
  }
  key.resize(keyLen);
  return key;
}

std::vector<uint8_t> toBytes(std::string_view value) {
  return std::vector<uint8_t>(
      reinterpret_cast<const uint8_t*>(value.data()),
      reinterpret_cast<const uint8_t*>(value.data()) + value.size()
  );
}

std::string toString(const std::vector<uint8_t>& bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

catapult::MoqtCompoundMatch toCatapultMatch(const std::vector<MatchRule>& rules) {
  if (rules.empty()) {
    return catapult::MoqtCompoundMatch::any();
  }
  std::vector<catapult::MoqtBinaryMatch> conditions;
  conditions.reserve(rules.size());
  for (const auto& rule : rules) {
    switch (rule.type) {
    case MatchRule::Type::Exact:
      conditions.push_back(catapult::MoqtBinaryMatch::exact(rule.value));
      break;
    case MatchRule::Type::Prefix:
      conditions.push_back(catapult::MoqtBinaryMatch::prefix(rule.value));
      break;
    case MatchRule::Type::Suffix:
      conditions.push_back(catapult::MoqtBinaryMatch::suffix(rule.value));
      break;
    case MatchRule::Type::Contains:
      conditions.push_back(catapult::MoqtBinaryMatch::contains(rule.value));
      break;
    }
  }
  return catapult::MoqtCompoundMatch::all(std::move(conditions));
}

MatchRule::Type fromCatapultMatchType(catapult::BinaryMatchType type) {
  switch (type) {
  case catapult::BinaryMatchType::EXACT:
    return MatchRule::Type::Exact;
  case catapult::BinaryMatchType::PREFIX:
    return MatchRule::Type::Prefix;
  case catapult::BinaryMatchType::SUFFIX:
    return MatchRule::Type::Suffix;
  case catapult::BinaryMatchType::CONTAINS:
    return MatchRule::Type::Contains;
  }
  return MatchRule::Type::Exact;
}

std::vector<MatchRule> fromCatapultMatch(const catapult::MoqtCompoundMatch& match) {
  if (match.is_empty()) {
    return {};
  }
  std::vector<MatchRule> rules;
  rules.reserve(match.conditions().size());
  for (const auto& cond : match.conditions()) {
    rules.push_back(MatchRule{
        .type = fromCatapultMatchType(cond.match_type),
        .value = toString(cond.pattern),
    });
  }
  return rules;
}

catapult::CatToken tokenFromGrants(const Grants& grants) {
  catapult::CatToken token;
  if (grants.expiresAt != std::chrono::system_clock::time_point::max()) {
    token.core.exp =
        std::chrono::duration_cast<std::chrono::seconds>(grants.expiresAt.time_since_epoch())
            .count();
  }

  catapult::MoqtClaims moqt = catapult::MoqtClaims::create(grants.scopes.size());
  for (const auto& scope : grants.scopes) {
    std::vector<int> actions;
    actions.reserve(scope.actions.size());
    for (auto action : scope.actions) {
      actions.push_back(static_cast<int>(action));
    }
    moqt.addScope(
        actions,
        toCatapultMatch(scope.namespaceMatches),
        toCatapultMatch(scope.trackMatches)
    );
  }
  token.extended.setMoqtClaims(std::move(moqt));
  token.validateTokenStructure();
  return token;
}

Grants grantsFromToken(const catapult::CatToken& token) {
  Grants grants;
  if (token.core.exp.has_value()) {
    grants.expiresAt = std::chrono::system_clock::time_point(std::chrono::seconds(*token.core.exp));
  }

  const auto* moqt = token.extended.getMoqtClaimsReadOnly();
  if (!moqt) {
    return grants;
  }

  for (const auto& catScope : moqt->getScopes()) {
    Scope scope;
    scope.actions.reserve(catScope.actions.size());
    for (auto action : catScope.actions) {
      if (action < 0) {
        continue;
      }
      scope.actions.push_back(static_cast<Action>(static_cast<uint64_t>(action)));
    }
    scope.namespaceMatches = fromCatapultMatch(catScope.namespace_match);
    scope.trackMatches = fromCatapultMatch(catScope.track_match);
    grants.scopes.push_back(std::move(scope));
  }
  return grants;
}

} // namespace

AuthTokenVerifier::AuthTokenVerifier(config::AuthConfig config) : config_(std::move(config)) {
  derivedKeys_.reserve(config_.hmacKeys.size());
  for (const auto& key : config_.hmacKeys) {
    const std::size_t idx = derivedKeys_.size();
    derivedKeys_.push_back(DerivedKey{.id = key.id, .key = deriveHmacKey(key.secret)});
    if (!key.id.empty()) {
      keyIdIndex_.emplace(key.id, idx);
    }
  }
}

folly::Expected<Grants, AuthError> AuthTokenVerifier::verify(const AuthToken& token) const {
  if (!config_.enabled) {
    return Grants{};
  }
  if (token.tokenType != config_.tokenType) {
    return folly::makeUnexpected(AuthError::WrongTokenType);
  }
  if (token.tokenValue.empty()) {
    return folly::makeUnexpected(AuthError::Malformed);
  }

  const auto tokenBytes = toBytes(token.tokenValue);
  const auto span = std::span<const uint8_t>(tokenBytes.data(), tokenBytes.size());

  // Peek the COSE protected header to extract kid without any crypto.
  std::optional<std::string> tokenKid;
  try {
    auto header = catapult::Cwt::decodeHeader(span);
    tokenKid = header.kid;
  } catch (const catapult::CatError&) {
    return folly::makeUnexpected(AuthError::Malformed);
  }

  auto tryVerify = [&](const DerivedKey& derived) -> folly::Expected<Grants, AuthError> {
    try {
      catapult::HmacSha256Algorithm hmac(derived.key);
      auto cwt = catapult::Cwt::validateCwt(span, hmac);
      auto grants = grantsFromToken(cwt.payload);
      if (grants.expiresAt <= std::chrono::system_clock::now()) {
        return folly::makeUnexpected(AuthError::Expired);
      }
      return grants;
    } catch (const catapult::CryptoError&) {
      return folly::makeUnexpected(AuthError::BadSignature);
    } catch (const catapult::CatError&) {
      return folly::makeUnexpected(AuthError::Malformed);
    }
  };

  if (tokenKid.has_value()) {
    auto it = keyIdIndex_.find(*tokenKid);
    if (it == keyIdIndex_.end()) {
      return folly::makeUnexpected(AuthError::BadSignature);
    }
    return tryVerify(derivedKeys_[it->second]);
  }

  // No kid — trial-verify against all configured keys.
  for (const auto& derived : derivedKeys_) {
    auto result = tryVerify(derived);
    if (result.hasValue() || result.error() != AuthError::BadSignature) {
      return result;
    }
  }
  return folly::makeUnexpected(AuthError::BadSignature);
}

std::string AuthTokenVerifier::signForTest(
    std::string_view keyID,
    std::string_view secret,
    const Grants& grants
) {
  catapult::HmacSha256Algorithm hmac(deriveHmacKey(secret));
  catapult::Cwt cwt(catapult::ALG_HMAC256_256, tokenFromGrants(grants));
  cwt.withKeyId(std::string(keyID));
  return toString(cwt.createCwt(catapult::CwtMode::MACed, hmac));
}

std::optional<AuthToken> findAuthToken(const Parameters& params, uint64_t tokenType) {
  const auto authKey = folly::to_underlying(TrackRequestParamKey::AUTHORIZATION_TOKEN);
  for (const auto& param : params) {
    if (param.key == authKey && param.asAuthToken.tokenType == tokenType) {
      return param.asAuthToken;
    }
  }
  return std::nullopt;
}

namespace {

// Shared implementation for both allows() overloads. A namespace-level check
// passes std::nullopt for trackName (the track match rules then see empty bytes).
bool allowsImpl(
    const Grants& grants,
    Action action,
    const TrackNamespace& ns,
    std::optional<std::string_view> trackName,
    std::chrono::system_clock::time_point now
) {
  if (grants.expiresAt <= now) {
    return false;
  }
  const auto nsBytes = canonicalNamespace(ns);
  const auto trackBytes = trackName.value_or(std::string_view());
  for (const auto& scope : grants.scopes) {
    if (std::find(scope.actions.begin(), scope.actions.end(), action) == scope.actions.end()) {
      continue;
    }
    auto rulesMatch = [](std::string_view actual, const std::vector<MatchRule>& rules) {
      return std::all_of(rules.begin(), rules.end(), [&](const auto& rule) {
        const auto expected = std::string_view(rule.value);
        switch (rule.type) {
        case MatchRule::Type::Exact:
          return actual == expected;
        case MatchRule::Type::Prefix:
          return actual.starts_with(expected);
        case MatchRule::Type::Suffix:
          return actual.size() >= expected.size() &&
                 actual.substr(actual.size() - expected.size()) == expected;
        case MatchRule::Type::Contains:
          return actual.find(expected) != std::string_view::npos;
        }
        return false;
      });
    };
    if (rulesMatch(nsBytes, scope.namespaceMatches) && rulesMatch(trackBytes, scope.trackMatches)) {
      return true;
    }
  }
  return false;
}

} // namespace

bool allows(
    const Grants& grants,
    Action action,
    const TrackNamespace& ns,
    std::chrono::system_clock::time_point now
) {
  return allowsImpl(grants, action, ns, std::nullopt, now);
}

bool allows(
    const Grants& grants,
    Action action,
    const FullTrackName& ftn,
    std::chrono::system_clock::time_point now
) {
  return allowsImpl(grants, action, ftn.trackNamespace, std::string_view(ftn.trackName), now);
}

const char* toString(AuthError error) {
  switch (error) {
  case AuthError::Missing:
    return "missing authorization token";
  case AuthError::WrongTokenType:
    return "wrong authorization token type";
  case AuthError::Malformed:
    return "malformed authorization token";
  case AuthError::BadSignature:
    return "invalid authorization token signature";
  case AuthError::Expired:
    return "expired authorization token";
  case AuthError::Forbidden:
    return "authorization token does not permit action";
  }
  return "authorization failed";
}

} // namespace openmoq::moqx::auth
