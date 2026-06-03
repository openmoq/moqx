/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "config/Config.h"

#include <folly/Expected.h>
#include <moxygen/MoQTypes.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace openmoq::moqx::auth {

enum class Action : uint64_t {
  ClientSetup = 0,
  ServerSetup = 1,
  PublishNamespace = 2,
  SubscribeNamespace = 3,
  Subscribe = 4,
  RequestUpdate = 5,
  Publish = 6,
  Fetch = 7,
  TrackStatus = 8,
};

enum class AuthError {
  Missing,
  WrongTokenType,
  Malformed,
  BadSignature,
  Expired,
  Forbidden,
};

struct MatchRule {
  enum class Type : uint64_t { Exact = 0, Prefix = 1, Suffix = 2, Contains = 3 };
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

class AuthTokenVerifier {
public:
  explicit AuthTokenVerifier(config::AuthConfig config);

  bool enabled() const { return config_.enabled; }
  uint64_t tokenType() const { return config_.tokenType; }
  bool requireSetupToken() const { return config_.requireSetupToken; }
  bool allowRequestTokenOverride() const { return config_.allowRequestTokenOverride; }

  folly::Expected<Grants, AuthError> verify(const moxygen::AuthToken& token) const;

  // Test/local issuer helper for Catapult CWT tokens signed with the configured HMAC key.
  static std::string
  signForTest(std::string_view keyID, std::string_view secret, const Grants& grants);

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
};

std::optional<moxygen::AuthToken>
findAuthToken(const moxygen::Parameters& params, uint64_t tokenType);

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

const char* toString(AuthError error);

} // namespace openmoq::moqx::auth
