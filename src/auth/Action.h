/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "util/ExhaustiveSwitch.h"

#include <folly/String.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// CAT4MOQ actions and their canonical names. Lightweight (moxygen-free) on
// purpose.
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

enum class MatchRuleType : uint64_t { Exact = 0, Prefix = 1, Suffix = 2, Contains = 3 };

ENFORCE_EXHAUSTIVE_SWITCH_BEGIN
inline std::string_view actionName(Action action) {
  switch (action) {
  case Action::ClientSetup:
    return "client_setup";
  case Action::ServerSetup:
    return "server_setup";
  case Action::PublishNamespace:
    return "publish_namespace";
  case Action::SubscribeNamespace:
    return "subscribe_namespace";
  case Action::Subscribe:
    return "subscribe";
  case Action::RequestUpdate:
    return "request_update";
  case Action::Publish:
    return "publish";
  case Action::Fetch:
    return "fetch";
  case Action::TrackStatus:
    return "track_status";
  }
  return "unknown";
}
ENFORCE_EXHAUSTIVE_SWITCH_END

// Canonicalizes an action name (aliases, numeric IDs, case/dash-insensitive)
// to its Action, or nullopt if unrecognized.
inline std::optional<Action> canonicalAction(std::string_view name) {
  std::string normalized =
      folly::trimWhitespace(folly::StringPiece(name.data(), name.size())).str();
  std::replace(normalized.begin(), normalized.end(), '-', '_');
  std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });

  if (normalized == "client_setup" || normalized == "setup" || normalized == "0") {
    return Action::ClientSetup;
  }
  if (normalized == "server_setup" || normalized == "1") {
    return Action::ServerSetup;
  }
  if (normalized == "publish_namespace" || normalized == "announce" || normalized == "2") {
    return Action::PublishNamespace;
  }
  if (normalized == "subscribe_namespace" || normalized == "3") {
    return Action::SubscribeNamespace;
  }
  if (normalized == "subscribe" || normalized == "4") {
    return Action::Subscribe;
  }
  if (normalized == "request_update" || normalized == "subscribe_update" || normalized == "5") {
    return Action::RequestUpdate;
  }
  if (normalized == "publish" || normalized == "6") {
    return Action::Publish;
  }
  if (normalized == "fetch" || normalized == "7") {
    return Action::Fetch;
  }
  if (normalized == "track_status" || normalized == "8") {
    return Action::TrackStatus;
  }
  return std::nullopt;
}

} // namespace openmoq::moqx::auth
