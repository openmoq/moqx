/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "auth/Action.h"

#include <chrono>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace catapult {
class CryptographicAlgorithm;
}

namespace openmoq::moqx::auth {

constexpr uint64_t kC4mTokenType = 0x01;
// Unassigned in draft 02; these are the Private Use labels in Appendix A.
constexpr int64_t kC4mMoqtClaim = -65537;

enum class AuthError {
  Missing,
  WrongTokenType,
  Malformed,
  BadSignature,
  Expired,
  Forbidden,
  TooManyTokens,
  NotYetValid,
  UnsupportedClaim,
};

struct MatchRule {
  using Type = MatchRuleType;
  Type type{Type::Exact};
  std::string value;
};

struct Scope {
  std::vector<Action> actions;
  // One rule per namespace field; exactNamespace represents the final nil.
  std::vector<MatchRule> namespaceMatches;
  // C4M permits at most one track match. A vector preserves the internal grant API.
  std::vector<MatchRule> trackMatches;
  bool exactNamespace{false};
};

struct Grants {
  std::chrono::system_clock::time_point expiresAt{std::chrono::system_clock::time_point::max()};
  std::vector<Scope> scopes;
  // CWT iss (1) and aud (3). Only encoded when set; the relay validates them against ClaimPolicy.
  std::optional<std::string> issuer;
  std::vector<std::string> audiences;
};

// Relay-side validation of standard CWT claims (draft-02 section 8.2.1).
struct ClaimPolicy {
  // Also reject informational claims that the relay does not validate.
  bool strict{false};
  // When non-empty, aud is required and at least one token audience must equal one entry.
  // When empty, a token carrying aud is rejected because its semantics cannot be honored.
  std::vector<std::string> audiences;
  // When non-empty, iss is required and must equal one entry.
  std::vector<std::string> issuers;
};

class C4mError final : public std::runtime_error {
public:
  C4mError(AuthError code, const char* message) : std::runtime_error(message), code_(code) {}
  AuthError code() const { return code_; }

private:
  AuthError code_;
};

// The unverified kid is only for routing; verifyC4mToken authenticates the full header.
struct C4mHeader {
  int64_t algorithm{0};
  std::optional<std::string> keyId;
};
C4mHeader c4mHeader(std::string_view token);
std::optional<std::string> c4mKeyId(std::string_view token);
Grants verifyC4mToken(
    std::string_view token,
    const catapult::CryptographicAlgorithm& algorithm,
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now(),
    const ClaimPolicy& policy = {}
);
Grants verifyC4mToken(
    std::string_view token,
    std::span<const uint8_t> key,
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now(),
    const ClaimPolicy& policy = {}
);
std::string
signC4mToken(std::string_view keyId, std::span<const uint8_t> key, const Grants& grants);
std::string signC4mToken(
    std::string_view keyId,
    const catapult::CryptographicAlgorithm& algorithm,
    const Grants& grants
);
bool matchesC4mScope(
    const Scope& scope,
    Action action,
    std::span<const std::string> trackNamespace,
    std::optional<std::string_view> trackName
);

} // namespace openmoq::moqx::auth
