/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "auth/Action.h"
#include "auth/AuthConfig.h"

#include <folly/Expected.h>
#include <folly/Unit.h>
#include <folly/lang/Bits.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace openmoq::moqx::auth {

enum class AuthError {
  Missing,
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

// A track namespace: its segments in order.
using Namespace = std::span<const std::string>;

// The values of a message's AUTHORIZATION_TOKENs of the configured type.
using TokenValues = std::vector<std::string>;

// Length-prefixed byte-encoding of a namespace's segments, used both as a
// MatchRule value and as the bytes matched against it. A stable wire format:
// signer and verifier must produce byte-identical output independently.
inline std::string canonicalNamespace(Namespace ns) {
  std::string out;
  for (const auto& field : ns) {
    const uint32_t lenBigEndian = folly::Endian::big(static_cast<uint32_t>(field.size()));
    out.append(reinterpret_cast<const char*>(&lenBigEndian), sizeof(lenBigEndian));
    out.append(field);
  }
  return out;
}

class AuthTokenVerifier {
public:
  explicit AuthTokenVerifier(AuthConfig config);

  uint64_t tokenType() const { return config_.tokenType; }
  bool requireSetupToken() const { return config_.requireSetupToken; }
  bool allowRequestTokenOverride() const { return config_.allowRequestTokenOverride; }

  // Caps how many AUTHORIZATION_TOKEN params of the configured tokenType()
  // one message may carry; a message over the cap is rejected outright, not
  // truncated. Params of other token types are not counted. 0 would reject
  // every message carrying a matching token. Config validation requires >=1.
  uint32_t maxTokensPerMessage() const { return config_.maxTokensPerMessage; }

  // Statically-configured anonymous claim, applied as a floor on every
  // request (see docs/config.md#anonymous-claim). Never covers
  // Action::ClientSetup — it can't bypass a required setup token.
  const Grants& anonymousGrants() const { return anonymousGrants_; }

  // `token` is the value of one token of tokenType().
  folly::Expected<Grants, AuthError> verify(std::string_view token) const;

private:
  // HMAC key material derived once at construction. keyIdIndex_ maps each
  // configured key's id to its index in derivedKeys_, enabling O(1) key
  // selection when a token carries a kid header. Tokens without a kid fall
  // back to trial-verification over the full derivedKeys_ list.
  struct DerivedKey {
    std::string id;
    std::vector<uint8_t> key;
  };

  AuthConfig config_;
  std::vector<DerivedKey> derivedKeys_;
  std::unordered_map<std::string, std::size_t> keyIdIndex_;
  Grants anonymousGrants_;
};

// Namespace-level authorization (e.g. PublishNamespace, SubscribeNamespace, setup).
bool allows(
    const Grants& grants,
    Action action,
    Namespace ns,
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now()
);

// Track-level authorization (e.g. Subscribe, Publish, Fetch, TrackStatus).
bool allows(
    const Grants& grants,
    Action action,
    Namespace ns,
    std::string_view track,
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now()
);

// True if any element of grantsList allows the action (namespace-level).
bool allowsAny(
    const std::vector<Grants>& grantsList,
    Action action,
    Namespace ns,
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now()
);

// True if any element of grantsList allows the action (track-level).
bool allowsAny(
    const std::vector<Grants>& grantsList,
    Action action,
    Namespace ns,
    std::string_view track,
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now()
);

const char* toString(AuthError error);

// Verifies the setup tokens and returns every verified token's grants. When
// requireSetupToken() is set, connecting is additionally gated on whether any
// one of them permits Action::ClientSetup; without it, verified grants pool
// into the session with no ClientSetup check.
folly::Expected<std::vector<Grants>, AuthError>
authenticateSetup(const AuthTokenVerifier& verifier, const TokenValues& setupTokens);

// Authorizes a request against session grants, or per-request token(s) when
// allow_request_token_override is set. Permitted if any verified request
// token, or any session grant, covers the action. Returns Unit when permitted.
folly::Expected<folly::Unit, AuthError> authorize(
    const AuthTokenVerifier& verifier,
    Action action,
    const TokenValues& requestTokens,
    Namespace ns,
    const std::vector<Grants>& sessionGrants,
    std::optional<std::string_view> trackName = std::nullopt
);

} // namespace openmoq::moqx::auth
