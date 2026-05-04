/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "auth/Auth.h"

#include <catapult/crypto.hpp>
#include <catapult/cwt.hpp>
#include <catapult/error.hpp>
#include <catapult/moqt_claims.hpp>
#include <folly/Conv.h>
#include <folly/Expected.h>
#include <folly/Range.h>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <limits>
#include <span>
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

std::vector<uint8_t> deriveHmacKey(std::string_view secret) {
  std::vector<uint8_t> key(SHA256_DIGEST_LENGTH);
  SHA256(reinterpret_cast<const unsigned char*>(secret.data()), secret.size(), key.data());
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

catapult::MoqtBinaryMatch toCatapultMatch(const std::vector<MatchRule>& rules) {
  if (rules.empty()) {
    return catapult::MoqtBinaryMatch::any();
  }
  const auto& rule = rules.front();
  switch (rule.type) {
  case MatchRule::Type::Exact:
    return catapult::MoqtBinaryMatch::exact(rule.value);
  case MatchRule::Type::Prefix:
    return catapult::MoqtBinaryMatch::prefix(rule.value);
  case MatchRule::Type::Suffix:
    return catapult::MoqtBinaryMatch::suffix(rule.value);
  case MatchRule::Type::Contains:
    return catapult::MoqtBinaryMatch::contains(rule.value);
  }
  return catapult::MoqtBinaryMatch::any();
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

std::vector<MatchRule> fromCatapultMatch(const catapult::MoqtBinaryMatch& match) {
  if (match.is_empty()) {
    return {};
  }
  return {MatchRule{
      .type = fromCatapultMatchType(match.match_type),
      .value = toString(match.pattern),
  }};
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

AuthTokenVerifier::AuthTokenVerifier(config::AuthConfig config) : config_(std::move(config)) {}

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
  for (const auto& key : config_.hmacKeys) {
    try {
      catapult::HmacSha256Algorithm hmac(deriveHmacKey(key.secret));
      auto cwt = catapult::Cwt::validateCwt(
          std::span<const uint8_t>(tokenBytes.data(), tokenBytes.size()),
          hmac
      );
      auto grants = grantsFromToken(cwt.payload);
      if (grants.expiresAt <= std::chrono::system_clock::now()) {
        return folly::makeUnexpected(AuthError::Expired);
      }
      return grants;
    } catch (const catapult::CryptoError&) {
      continue;
    } catch (const catapult::CatError&) {
      return folly::makeUnexpected(AuthError::Malformed);
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

bool allows(
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
