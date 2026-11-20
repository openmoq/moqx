/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "auth/Auth.h"
#include "auth/Es256Key.h"
#include "auth/HmacKey.h"

#include <catapult/crypto.hpp>

#include <folly/Conv.h>
#include <folly/Expected.h>
#include <folly/Range.h>
#include <folly/logging/xlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace moxygen;

namespace openmoq::moqx::auth {
namespace {

struct VerifiedTokens {
  std::vector<Grants> grants;
  std::optional<AuthError> firstError;
};

// Picks the error for a denial after token verification: with at least one
// verified token the problem is scope (Forbidden), not the crypto/format
// problem `firstError` holds for the *other* tokens that failed to verify.
AuthError
denialError(const std::vector<Grants>& verifiedGrants, std::optional<AuthError> firstError) {
  return verifiedGrants.empty() ? firstError.value_or(AuthError::Forbidden) : AuthError::Forbidden;
}

// Verifies every token, pooling grants from the ones that succeed. A token
// that fails to verify is dropped as a non-viable candidate; `firstError`
// keeps the earliest failure for a caller with nothing else to report.
VerifiedTokens verifyTokens(
    const AuthTokenVerifier& verifier,
    const std::vector<AuthToken>& tokens,
    uint64_t connectionId
) {
  VerifiedTokens result;
  for (const auto& token : tokens) {
    auto verified = verifier.verify(token, connectionId);
    if (verified.hasError()) {
      if (!result.firstError) {
        result.firstError = verified.error();
      }
      continue;
    }
    result.grants.push_back(std::move(verified.value()));
  }
  return result;
}

} // namespace

AuthTokenVerifier::AuthTokenVerifier(config::AuthConfig config) : config_(std::move(config)) {
  claimPolicy_ = ClaimPolicy{
      .strict = config_.strictClaims,
      .audiences = config_.audiences,
      .issuers = config_.issuers,
  };
  if (!config_.enabled) {
    return;
  }
  keys_.reserve(config_.hmacKeys.size() + config_.es256Keys.size());
  auto addKey = [&](std::string id,
                    std::shared_ptr<const catapult::CryptographicAlgorithm> algorithm) {
    if (id.empty() || !keyIdIndex_.emplace(id, keys_.size()).second) {
      throw std::invalid_argument("auth key IDs must be non-empty and unique across algorithms");
    }
    keys_.push_back(VerificationKey{.id = std::move(id), .algorithm = std::move(algorithm)});
  };
  for (const auto& key : config_.hmacKeys) {
    addKey(key.id, std::make_shared<catapult::HmacSha256Algorithm>(deriveHmacKey(key.secret)));
  }
  for (const auto& key : config_.es256Keys) {
    addKey(key.id, loadEs256PublicKeyFile(key.publicKeyFile));
  }
}

uint64_t newConnectionId() {
  static std::atomic<uint64_t> next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
}

folly::Expected<Grants, AuthError>
AuthTokenVerifier::verify(const AuthToken& token, uint64_t connectionId) const {
  auto result = verifySignature(token);
  if (result.hasValue() && result.value().dpop) {
    result.value().tokenHash = sha256(token.tokenValue);
  }
  if (result.hasValue() && result.value().replayProtected &&
      !bindReplay(result.value(), connectionId)) {
    return folly::makeUnexpected(AuthError::Replayed);
  }
  return result;
}

bool AuthTokenVerifier::noteProofCti(
    const Grants& grants,
    const std::string& cti,
    std::chrono::system_clock::time_point expiresAt
) const {
  std::string key = grants.tokenHash + cti;
  const auto now = std::chrono::system_clock::now();
  std::lock_guard lock(proofMutex_);
  if (auto it = proofCtis_.find(key); it != proofCtis_.end()) {
    if (it->second > now) {
      return false;
    }
    it->second = expiresAt;
    return true;
  }
  if (proofCtis_.size() >= kMaxReplayEntries) {
    std::erase_if(proofCtis_, [&](const auto& e) { return e.second <= now; });
    if (proofCtis_.size() >= kMaxReplayEntries) {
      XLOG(WARN) << "DPoP replay state full; refusing proof";
      return false;
    }
  }
  proofCtis_.emplace(std::move(key), expiresAt);
  return true;
}

std::vector<Grants> AuthTokenVerifier::applyDpop(
    const std::vector<Grants>& grants,
    const Parameters& params,
    const DpopContext& context
) const {
  std::vector<Grants> usable;
  std::vector<AuthToken> proofs;
  bool proofsLoaded = false;
  const auto now = std::chrono::system_clock::now();
  for (const auto& grant : grants) {
    if (!grant.dpop) {
      usable.push_back(grant);
      continue;
    }
    if (!proofsLoaded) {
      proofs = findAuthTokens(params, config_.dpopTokenType);
      if (proofs.size() > config_.maxTokensPerMessage) {
        proofs.clear(); // Over the cap: treat as no usable proof rather than verify them all.
      }
      proofsLoaded = true;
    }
    for (const auto& proof : proofs) {
      try {
        // The proof names its token by hash, so only the matching grant's key will pass.
        const auto verified =
            verifyDpopProof(proof.tokenValue, grant.tokenHash, *grant.dpop, context, now);
        if (grant.dpop->detectReplay &&
            !noteProofCti(grant, verified.cti, verified.issuedAt + grant.dpop->window * 2)) {
          continue;
        }
        usable.push_back(grant);
        break;
      } catch (const C4mError&) {
        // Not a proof for this token; try the next one.
      }
    }
  }
  return usable;
}

// Binds a signature-verified, replay-protected token to its first connection. Binding happens
// only after the signature verifies so an unauthenticated peer cannot reserve arbitrary cti values.
bool AuthTokenVerifier::bindReplay(const Grants& grants, uint64_t connectionId) const {
  if (connectionId == 0 || !grants.cti) {
    return false;
  }
  std::string key = grants.issuer.value_or(std::string()) + '\0' + *grants.cti;
  const auto now = std::chrono::system_clock::now();
  std::lock_guard lock(replayMutex_);
  if (auto it = replay_.find(key); it != replay_.end()) {
    if (it->second.expiresAt > now) {
      return it->second.connectionId == connectionId;
    }
    replay_.erase(it);
  }
  if (replay_.size() >= kMaxReplayEntries) {
    std::erase_if(replay_, [&](const auto& e) { return e.second.expiresAt <= now; });
    if (replay_.size() >= kMaxReplayEntries) {
      XLOG(WARN) << "replay state full; refusing catreplay token";
      return false;
    }
  }
  replay_.emplace(std::move(key), ReplayEntry{connectionId, grants.expiresAt});
  return true;
}

folly::Expected<Grants, AuthError> AuthTokenVerifier::verifySignature(const AuthToken& token
) const {
  if (!config_.enabled) {
    return Grants{};
  }
  if (token.tokenType != config_.tokenType) {
    return folly::makeUnexpected(AuthError::WrongTokenType);
  }
  if (token.tokenValue.empty()) {
    return folly::makeUnexpected(AuthError::Malformed);
  }

  C4mHeader header;
  try {
    header = c4mHeader(token.tokenValue);
  } catch (const C4mError& error) {
    return folly::makeUnexpected(error.code());
  }

  auto tryVerify = [&](const VerificationKey& key) -> folly::Expected<Grants, AuthError> {
    try {
      return verifyC4mToken(
          token.tokenValue,
          *key.algorithm,
          std::chrono::system_clock::now(),
          claimPolicy_
      );
    } catch (const C4mError& error) {
      return folly::makeUnexpected(error.code());
    }
  };

  if (header.keyId.has_value()) {
    auto it = keyIdIndex_.find(*header.keyId);
    if (it == keyIdIndex_.end()) {
      return folly::makeUnexpected(AuthError::BadSignature);
    }
    return tryVerify(keys_[it->second]);
  }

  // No kid — trial-verify against all configured keys.
  for (const auto& key : keys_) {
    if (key.algorithm->algorithmId() != header.algorithm) {
      continue;
    }
    auto result = tryVerify(key);
    if (result.hasValue() || result.error() != AuthError::BadSignature) {
      return result;
    }
  }
  return folly::makeUnexpected(AuthError::BadSignature);
}

bool AuthTokenVerifier::allowsAnonymous(
    Action action,
    const TrackNamespace& ns,
    std::optional<std::string_view> trackName
) const {
  if (action == Action::ClientSetup) {
    return false;
  }
  const auto nsBytes = canonicalNamespace(ns);
  const auto matches = [](std::string_view actual, MatchRuleType mode, std::string_view value) {
    switch (mode) {
    case MatchRuleType::Exact:
      return actual == value;
    case MatchRuleType::Prefix:
      return actual.starts_with(value);
    case MatchRuleType::Suffix:
      return actual.ends_with(value);
    case MatchRuleType::Contains:
      return actual.find(value) != std::string_view::npos;
    }
    return false;
  };
  for (const auto& scope : config_.anonymousClaim) {
    if (std::find(scope.actions.begin(), scope.actions.end(), action) == scope.actions.end()) {
      continue;
    }
    if (scope.namespaceSegments && !matches(
                                       nsBytes,
                                       scope.namespaceMatchMode,
                                       canonicalNamespace(TrackNamespace{*scope.namespaceSegments})
                                   )) {
      continue;
    }
    if (scope.trackName &&
        (!trackName || !matches(*trackName, scope.trackMatchMode, *scope.trackName))) {
      continue;
    }
    return true;
  }
  return false;
}

folly::Expected<std::shared_ptr<const std::vector<Grants>>, AuthError> authenticateSetup(
    const AuthTokenVerifier& verifier,
    const Parameters& setupParams,
    uint64_t connectionId
) {
  if (!verifier.enabled()) {
    return std::shared_ptr<const std::vector<Grants>>{};
  }

  auto tokens = findAuthTokens(setupParams, verifier.tokenType());
  if (tokens.size() > verifier.maxTokensPerMessage()) {
    XLOG(WARN) << "authenticateSetup: " << tokens.size() << " setup tokens exceeds "
               << "max_tokens_per_message=" << verifier.maxTokensPerMessage() << "; rejecting";
    return folly::makeUnexpected(AuthError::TooManyTokens);
  }
  auto [verifiedGrants, firstError] = verifyTokens(verifier, tokens, connectionId);
  // A cnf-bound token only counts when this SETUP carries a proof for it (section 4.3).
  verifiedGrants =
      verifier.applyDpop(verifiedGrants, setupParams, DpopContext{Action::ClientSetup, {}, {}});

  if (verifier.requireSetupToken()) {
    if (tokens.empty()) {
      return folly::makeUnexpected(AuthError::Missing);
    }
    // Session is authorized iff any verified setup token grants ClientSetup;
    // all verified tokens' scopes still pool into the session grants below,
    // not just the one that granted it.
    if (!allowsAny(verifiedGrants, Action::ClientSetup, TrackNamespace{})) {
      return folly::makeUnexpected(denialError(verifiedGrants, firstError));
    }
  }
  return std::make_shared<const std::vector<Grants>>(std::move(verifiedGrants));
}

folly::Expected<folly::Unit, AuthError> authorize(
    const AuthTokenVerifier& verifier,
    Action action,
    const Parameters& params,
    const TrackNamespace& ns,
    const std::vector<Grants>& sessionGrants,
    std::optional<std::string_view> trackName,
    uint64_t connectionId
) {
  if (!verifier.enabled()) {
    return folly::unit;
  }

  auto tokens = findAuthTokens(params, verifier.tokenType());
  if (tokens.size() > verifier.maxTokensPerMessage()) {
    XLOG(WARN) << "authorize: " << tokens.size() << " request tokens exceeds "
               << "max_tokens_per_message=" << verifier.maxTokensPerMessage()
               << " for action=" << static_cast<uint64_t>(action) << "; rejecting";
    return folly::makeUnexpected(AuthError::TooManyTokens);
  }

  std::vector<Grants> requestGrants;
  std::optional<AuthError> firstError;
  if (verifier.allowRequestTokenOverride()) {
    auto verified = verifyTokens(verifier, tokens, connectionId);
    requestGrants = std::move(verified.grants);
    firstError = verified.firstError;
  } else if (!tokens.empty()) {
    XLOG(DBG1) << "authorize: ignoring request AUTHORIZATION_TOKEN(s) for action="
               << static_cast<uint64_t>(action) << " (allow_request_token_override is disabled)";
  }

  // cnf-bound grants, from this message or the session, need a proof for this exact action.
  DpopContext dpopContext{
      action,
      ns.trackNamespace,
      trackName ? std::optional<std::string>(*trackName) : std::nullopt
  };
  requestGrants = verifier.applyDpop(requestGrants, params, dpopContext);
  std::vector<Grants> dpopSessionGrants;
  const std::vector<Grants>* sessionPool = &sessionGrants;
  if (std::any_of(sessionGrants.begin(), sessionGrants.end(), [](const Grants& g) {
        return g.dpop.has_value();
      })) {
    dpopSessionGrants = verifier.applyDpop(sessionGrants, params, dpopContext);
    sessionPool = &dpopSessionGrants;
  }

  // Any one of three sources is sufficient.
  const auto now = std::chrono::system_clock::now();
  bool permitted = false;
  if (trackName) {
    const FullTrackName ftn{ns, std::string(*trackName)};
    permitted = allowsAny(requestGrants, action, ftn, now) ||
                allowsAny(*sessionPool, action, ftn, now) ||
                verifier.allowsAnonymous(action, ns, trackName);
  } else {
    permitted = allowsAny(requestGrants, action, ns, now) ||
                allowsAny(*sessionPool, action, ns, now) ||
                verifier.allowsAnonymous(action, ns, std::nullopt);
  }
  if (!permitted) {
    XLOG(DBG1) << "authorize: action=" << static_cast<uint64_t>(action)
               << " not permitted for ns=" << ns;
    return folly::makeUnexpected(denialError(requestGrants, firstError));
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

// A namespace-level check passes std::nullopt for trackName (the track match
// rules then see empty bytes).
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
  return std::any_of(grants.scopes.begin(), grants.scopes.end(), [&](const Scope& scope) {
    return matchesC4mScope(scope, action, ns.trackNamespace, trackName);
  });
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
  case AuthError::NotYetValid:
    return "authorization token not yet valid";
  case AuthError::UnsupportedClaim:
    return "unsupported authorization token restriction";
  case AuthError::Forbidden:
    return "authorization token does not permit action";
  case AuthError::TooManyTokens:
    return "too many authorization tokens in message";
  case AuthError::Replayed:
    return "authorization token already bound to another connection";
  }
  return "authorization failed";
}

} // namespace openmoq::moqx::auth
