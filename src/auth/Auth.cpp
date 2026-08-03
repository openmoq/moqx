/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "auth/Auth.h"
#include "auth/CborReader.h"
#include "auth/HmacKey.h"

#include <catapult/crypto.hpp>
#include <catapult/cwt.hpp>
#include <catapult/error.hpp>
#include <catapult/moqt_claims.hpp>
#include <folly/Conv.h>
#include <folly/Expected.h>
#include <folly/Range.h>
#include <folly/logging/xlog.h>

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

std::vector<uint8_t> toBytes(std::string_view value) {
  return std::vector<uint8_t>(
      reinterpret_cast<const uint8_t*>(value.data()),
      reinterpret_cast<const uint8_t*>(value.data()) + value.size()
  );
}

std::string toString(const std::vector<uint8_t>& bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
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

folly::Expected<std::shared_ptr<const std::vector<Grants>>, AuthError>
authenticateSetup(const AuthTokenVerifier& verifier, const Parameters& setupParams) {
  if (!verifier.enabled()) {
    return std::shared_ptr<const std::vector<Grants>>{};
  }

  auto tokens = findAuthTokens(setupParams, verifier.tokenType());
  std::vector<Grants> verifiedGrants;
  std::optional<AuthError> firstError;
  for (const auto& token : tokens) {
    auto verified = verifier.verify(token);
    if (verified.hasError()) {
      if (!firstError) {
        firstError = verified.error();
      }
      continue;
    }
    verifiedGrants.push_back(std::move(verified.value()));
  }

  if (!tokens.empty()) {
    // At least one setup token was presented; the session is authorized iff
    // any of the verified ones grants ClientSetup. All verified tokens' other
    // scopes still pool into the session grants below, not just the one that
    // happened to grant ClientSetup.
    if (!allowsAny(verifiedGrants, Action::ClientSetup, TrackNamespace{})) {
      return folly::makeUnexpected(firstError.value_or(AuthError::Forbidden));
    }
    return std::make_shared<const std::vector<Grants>>(std::move(verifiedGrants));
  }

  if (verifier.requireSetupToken()) {
    return folly::makeUnexpected(AuthError::Missing);
  }
  // No setup token but not required: connect with empty session grants;
  // requests must then carry their own tokens to be authorized (see authorize()).
  return std::make_shared<const std::vector<Grants>>();
}

folly::Expected<folly::Unit, AuthError> authorize(
    const AuthTokenVerifier& verifier,
    Action action,
    const Parameters& params,
    const TrackNamespace& ns,
    const std::vector<Grants>& sessionGrants,
    std::optional<std::string_view> trackName
) {
  if (!verifier.enabled()) {
    return folly::unit;
  }

  std::vector<Grants> requestGrants;
  std::optional<AuthError> firstError;
  if (verifier.allowRequestTokenOverride()) {
    for (const auto& token : findAuthTokens(params, verifier.tokenType())) {
      auto res = verifier.verify(token);
      if (res.hasError()) {
        // Dropped as a non-viable candidate — another request token or a
        // session grant may still cover the action.
        if (!firstError) {
          firstError = res.error();
        }
        continue;
      }
      requestGrants.push_back(std::move(res.value()));
    }
  } else if (!findAuthTokens(params, verifier.tokenType()).empty()) {
    XLOG(DBG1) << "authorize: ignoring request AUTHORIZATION_TOKEN(s) for action="
               << static_cast<uint64_t>(action) << " (allow_request_token_override is disabled)";
  }

  const bool permitted =
      trackName ? (allowsAny(requestGrants, action, FullTrackName{ns, std::string(*trackName)}) ||
                   allowsAny(sessionGrants, action, FullTrackName{ns, std::string(*trackName)}))
                : (allowsAny(requestGrants, action, ns) || allowsAny(sessionGrants, action, ns));
  if (!permitted) {
    XLOG(DBG1) << "authorize: action=" << static_cast<uint64_t>(action)
               << " not permitted for ns=" << ns;
    return folly::makeUnexpected(firstError.value_or(AuthError::Forbidden));
  }
  return folly::unit;
}

std::vector<AuthToken> findAuthTokens(const Parameters& params, uint64_t tokenType) {
  const auto authKey = folly::to_underlying(TrackRequestParamKey::AUTHORIZATION_TOKEN);
  std::vector<AuthToken> tokens;
  for (const auto& param : params) {
    if (param.key == authKey && param.asAuthToken.tokenType == tokenType) {
      tokens.push_back(param.asAuthToken);
    }
  }
  return tokens;
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

bool allowsAny(
    const std::vector<Grants>& grantsList,
    Action action,
    const TrackNamespace& ns,
    std::chrono::system_clock::time_point now
) {
  return std::any_of(grantsList.begin(), grantsList.end(), [&](const Grants& grants) {
    return allows(grants, action, ns, now);
  });
}

bool allowsAny(
    const std::vector<Grants>& grantsList,
    Action action,
    const FullTrackName& ftn,
    std::chrono::system_clock::time_point now
) {
  return std::any_of(grantsList.begin(), grantsList.end(), [&](const Grants& grants) {
    return allows(grants, action, ftn, now);
  });
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
