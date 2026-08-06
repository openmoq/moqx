/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "auth/Action.h"
#include "config/Config.h"

#include <folly/Expected.h>
#include <folly/Unit.h>
#include <moxygen/MoQTypes.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace openmoq::moqx::auth {

enum class AuthError {
  Missing,
  WrongTokenType,
  Malformed,
  BadSignature,
  Expired,
  Forbidden,
  TooManyTokens,
};

struct MatchRule {
  using Type = MatchRuleType;
  Type type{Type::Exact};
  std::string value;
};

struct Scope {
  std::vector<Action> actions;
  std::vector<MatchRule> namespaceMatches;
  std::vector<MatchRule> trackMatches;
};

struct Grants {
  std::chrono::system_clock::time_point expiresAt{std::chrono::system_clock::time_point::max()};
  std::vector<Scope> scopes;
};

// Length-prefixed byte-encoding of a namespace's segments, used both as a
// MatchRule value and as the bytes matched against it. A stable wire format:
// signer and verifier must produce byte-identical output independently.
inline std::string canonicalNamespace(const moxygen::TrackNamespace& ns) {
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

class AuthTokenVerifier {
public:
  explicit AuthTokenVerifier(config::AuthConfig config);

  bool enabled() const { return config_.enabled; }
  uint64_t tokenType() const { return config_.tokenType; }
  bool requireSetupToken() const { return config_.requireSetupToken; }
  bool allowRequestTokenOverride() const { return config_.allowRequestTokenOverride; }

  // Caps how many AUTHORIZATION_TOKEN params of the configured tokenType()
  // one message may carry; a message over the cap is rejected outright, not
  // truncated. Params of other token types are not counted. 0 would reject
  // every message carrying a matching token. Config validation requires >=1
  // when enabled().
  uint32_t maxTokensPerMessage() const { return config_.maxTokensPerMessage; }

  // Statically-configured anonymous claim, applied as a floor on every
  // request (see docs/config.md#anonymous-claim). Never covers
  // Action::ClientSetup — it can't bypass a required setup token.
  const Grants& anonymousGrants() const { return anonymousGrants_; }

  folly::Expected<Grants, AuthError> verify(const moxygen::AuthToken& token) const;

private:
  // HMAC key material derived once at construction. keyIdIndex_ maps each
  // configured key's id to its index in derivedKeys_, enabling O(1) key
  // selection when a token carries a kid header. Tokens without a kid fall
  // back to trial-verification over the full derivedKeys_ list.
  struct DerivedKey {
    std::string id;
    std::vector<uint8_t> key;
  };

  config::AuthConfig config_;
  std::vector<DerivedKey> derivedKeys_;
  std::unordered_map<std::string, std::size_t> keyIdIndex_;
  Grants anonymousGrants_;
};

// Returns every AUTHORIZATION_TOKEN parameter matching tokenType, not just the
// first — a message may carry more than one, and a request is authorized if
// any one of them verifies and covers the action (see authorize()).
std::vector<moxygen::AuthToken>
findAuthTokens(const moxygen::Parameters& params, uint64_t tokenType);

// Namespace-level authorization (e.g. PublishNamespace, SubscribeNamespace, setup).
bool allows(
    const Grants& grants,
    Action action,
    const moxygen::TrackNamespace& ns,
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now()
);

// Track-level authorization (e.g. Subscribe, Publish, Fetch, TrackStatus).
bool allows(
    const Grants& grants,
    Action action,
    const moxygen::FullTrackName& ftn,
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now()
);

// True if any element of grantsList allows the action (namespace-level).
bool allowsAny(
    const std::vector<Grants>& grantsList,
    Action action,
    const moxygen::TrackNamespace& ns,
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now()
);

// True if any element of grantsList allows the action (track-level).
bool allowsAny(
    const std::vector<Grants>& grantsList,
    Action action,
    const moxygen::FullTrackName& ftn,
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now()
);

const char* toString(AuthError error);

// Verifies the setup AUTHORIZATION_TOKEN(s): null when auth is disabled,
// else every successfully-verified token's grants. When requireSetupToken()
// is set, connecting is additionally gated on whether any one of them
// permits Action::ClientSetup; without it, verified grants pool into the
// session with no ClientSetup check.
folly::Expected<std::shared_ptr<const std::vector<Grants>>, AuthError>
authenticateSetup(const AuthTokenVerifier& verifier, const moxygen::Parameters& setupParams);

// Authorizes a request against session grants, or per-request token(s) when
// allow_request_token_override is set. Permitted if any verified request
// token, or any session grant, covers the action. Returns Unit when permitted.
folly::Expected<folly::Unit, AuthError> authorize(
    const AuthTokenVerifier& verifier,
    Action action,
    const moxygen::Parameters& params,
    const moxygen::TrackNamespace& ns,
    const std::vector<Grants>& sessionGrants,
    std::optional<std::string_view> trackName = std::nullopt
);

} // namespace openmoq::moqx::auth
