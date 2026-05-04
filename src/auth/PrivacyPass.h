/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <folly/Expected.h>

#include <moxygen/MoQTypes.h>

#include "config/Config.h"

namespace openmoq::moqx::auth {

enum class Action : uint8_t {
  ClientSetup = 0,
  ServerSetup = 1,
  PublishNamespace = 2,
  SubscribeNamespace = 3,
  Subscribe = 4,
  SubscribeUpdate = 5,
  Publish = 6,
  Fetch = 7,
  TrackStatus = 8,
};

enum class MatchKind : uint8_t {
  Any = 0,
  Exact = 1,
  Prefix = 2,
  Suffix = 3,
  Contains = 4,
};

struct MatchSpec {
  MatchKind kind{MatchKind::Any};
  std::string value;

  bool matches(std::string_view target) const;
};

struct Scope {
  std::vector<Action> actions;
  MatchSpec namespaceMatch;
  MatchSpec trackMatch;
};

struct Claims {
  std::string issuerId;
  std::string audience;
  std::chrono::system_clock::time_point issuedAt;
  std::chrono::system_clock::time_point expiresAt;
  std::optional<std::chrono::seconds> revalidateAfter;
  std::vector<Scope> scopes;
};

enum class AuthError {
  Missing,
  Malformed,
  BadSignature,
  Expired,
  Forbidden,
  WrongTokenType,
};

std::string toString(AuthError error);

// Helper for tests and for external issuer-side fixtures.
std::string signTokenForTest(
    const config::AuthIssuerKey& issuerKey,
    std::string_view privateKeyPem,
    const Claims& claims
);

class PrivacyPassVerifier {
public:
  explicit PrivacyPassVerifier(config::AuthConfig config);

  bool enabled() const { return config_.enabled; }

  const std::string& audience() const { return config_.audience; }

  folly::Expected<Claims, AuthError> verify(const moxygen::AuthToken& token) const;

  bool allows(
      const Claims& claims,
      Action action,
      std::string_view namespaceValue,
      std::string_view trackValue = {}
  ) const;

  folly::Expected<folly::Unit, AuthError> authorize(
      const moxygen::Parameters& params,
      Action action,
      std::string_view namespaceValue,
      std::string_view trackValue = {}
  ) const;

  folly::Expected<folly::Unit, AuthError> authorizeSetup(const moxygen::SetupParameters& params
  ) const;

private:
  static constexpr std::string_view kMagic = "PPv1";

  folly::Expected<Claims, AuthError> tryVerifyToken(const moxygen::AuthToken& token) const;

  config::AuthConfig config_;
  std::vector<config::AuthIssuerKey> issuerKeys_;
};

} // namespace openmoq::moqx::auth
