/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "auth/Auth.h"

#include <moxygen/MoQTypes.h>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openmoq::moqx::auth {

struct IssueTokenOptions {
  std::string keyID;
  std::string secret;
  std::vector<Action> actions;
  moxygen::TrackNamespace trackNamespace;
  std::optional<std::string> trackName;
  std::chrono::seconds ttl{3600};
  std::chrono::system_clock::time_point now{std::chrono::system_clock::now()};
  std::optional<std::string> issuer;       // CWT iss
  std::vector<std::string> audiences;      // CWT aud
  bool replayProtected{false};             // catreplay 1 with a random cti: single connection
  std::chrono::seconds revalidateEvery{0}; // moqt-reval; 0 = none
  std::string dpopKeyFile;                 // PEM P-256 client key; binds the token (cnf)
  bool dpopJkt{false};                     // use jkt (RFC 7638) instead of ckt (RFC 9679)
  std::chrono::seconds dpopWindow{300};    // catdpop window
  std::string privateKeyFile;              // PEM P-256 private key; mutually exclusive with secret.
};

struct IssuedToken {
  std::string tokenValue;
};

IssuedToken issueToken(const IssueTokenOptions& options);

// Signs arbitrary grants into a CWT; kept out of moqx_core so the relay only verifies.
std::string signGrants(std::string_view keyID, std::string_view secret, const Grants& grants);

config::AuthConfig::HmacKey selectHmacKey(const config::AuthConfig& auth, std::string_view keyID);
std::vector<Action> parseActions(std::string_view value);
moxygen::TrackNamespace parseTrackNamespace(std::string_view value);

} // namespace openmoq::moqx::auth
