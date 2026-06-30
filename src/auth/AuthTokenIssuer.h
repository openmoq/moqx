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
};

struct IssuedToken {
  std::string tokenValue;
};

IssuedToken issueToken(const IssueTokenOptions& options);

config::AuthConfig::HmacKey selectHmacKey(const config::AuthConfig& auth, std::string_view keyID);
std::vector<Action> parseActions(std::string_view value);
moxygen::TrackNamespace parseTrackNamespace(std::string_view value);

} // namespace openmoq::moqx::auth
