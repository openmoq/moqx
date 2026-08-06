/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

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

// How a MatchRule (Auth.h) tests a candidate namespace/track name against its
// configured value. Lives here, not nested in MatchRule, so it's nameable
// without including Auth.h.
enum class MatchRuleType : uint64_t { Exact = 0, Prefix = 1, Suffix = 2, Contains = 3 };

#pragma GCC diagnostic push
#pragma GCC diagnostic error "-Wswitch"
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
#pragma GCC diagnostic pop

// Canonicalizes an action name (aliases, numeric IDs, case/dash-insensitive)
// to its Action, or nullopt if unrecognized. See docs/config.md's action
// table for the full name/alias list.
inline std::optional<Action> canonicalAction(std::string_view name) {
  auto begin = name.begin();
  auto end = name.end();
  while (begin != end && std::isspace(static_cast<unsigned char>(*begin))) {
    ++begin;
  }
  while (begin != end && std::isspace(static_cast<unsigned char>(*(end - 1)))) {
    --end;
  }
  std::string normalized(begin, end);
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
