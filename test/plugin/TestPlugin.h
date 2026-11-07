/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// A scriptable plugin type for tests. Every instance reports into one shared
// Script, which also decides how each hook answers.

#include "plugin/Registry.h"

#include <folly/Synchronized.h>
#include <folly/coro/Baton.h>

#include <atomic>
#include <string>
#include <vector>

namespace openmoq::moqx::plugin::test {

struct TestConfig {
  // How the instance names itself in Script events; tests set it to the id.
  std::optional<std::string> name;
  std::optional<std::string> secret;
  std::optional<bool> listener_params;
  std::optional<int64_t> tag;
};

struct TestState : SessionState {
  explicit TestState(std::string id) : id(std::move(id)) {}
  std::string id;
};

// Shared by every instance of the type; tests set it up before use.
struct Script {
  folly::Synchronized<std::vector<std::string>> events;

  // Hooks each instance implements.
  HookSet hooks{Hook::Admit | Hook::SessionEnded | Hook::Authorize};

  // Per plugin id: answer with this error instead of succeeding.
  folly::Synchronized<std::map<std::string, Error>> admitErrors;
  folly::Synchronized<std::map<std::string, Error>> authorizeErrors;
  folly::Synchronized<std::map<std::string, Error>> createErrors;
  folly::Synchronized<std::map<std::string, bool>> startThrows;

  // When set, admit / authorize wait for the baton (cancellation ends the wait).
  std::atomic<bool> suspendAdmit{false};
  std::atomic<bool> suspendAuthorize{false};
  folly::coro::Baton admitGate;
  folly::coro::Baton authorizeGate;
  // Ignore cancellation while suspended.
  std::atomic<bool> ignoreCancellation{false};

  // When set, admit records "param:<id>=<value>" for this SETUP key.
  std::optional<uint64_t> recordParam;

  std::map<std::string, Params> listenerParams;
  // When set, upstream setupParams waits for the gate, ignoring cancellation.
  std::atomic<bool> suspendUpstreamParams{false};
  folly::coro::Baton upstreamParamsGate;
  std::map<std::string, Params> upstreamParams;
  // Per plugin id: snapshot answer; absent means {"id": id}.
  folly::Synchronized<std::map<std::string, std::string>> snapshotThrows;
  std::atomic<bool> snapshotStalls{false};
  folly::coro::Baton snapshotGate;

  // Requests seen by authorize, as "<id>:<kind>:<track>:<peer>".
  folly::Synchronized<std::vector<std::string>> authorized;

  void record(std::string event) { events.wlock()->push_back(std::move(event)); }
  std::vector<std::string> take() { return std::exchange(*events.wlock(), {}); }
};

std::shared_ptr<Script>& currentScript();

// Allowed at both scopes. `tag` only exists for merge tests; `secret` is
// redacted.
std::shared_ptr<const PluginType> testType(std::string name = "test");

} // namespace openmoq::moqx::plugin::test
