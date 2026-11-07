/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// Level 1 plugin API: the control-plane hooks a compiled-in plugin implements.
// - Design: design/plugins-level1.md.
// - No moxygen type appears here. Views borrow the host's message.
// - Every hook is a coroutine. One that never suspends completes inline.
// - A view and the state passed to a hook stay valid until its task completes.

#include <folly/Executor.h>
#include <folly/Expected.h>
#include <folly/Unit.h>
#include <folly/coro/Task.h>
#include <folly/json/dynamic.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace openmoq::moqx::plugin {

struct Error {
  // The host maps a kind to the error code of the message it answers.
  enum class Kind {
    Unauthorized,
    Forbidden,
    ExpiredToken,
    MalformedToken,
    TooManyTokens,
    DoesNotExist,
    Internal,
  };
  Kind kind{Kind::Internal};
  // Sent to the peer.
  std::string reason;
};

template <class T> using Result = folly::Expected<T, Error>;

inline folly::Unexpected<Error> fail(Error::Kind kind, std::string reason) {
  return folly::makeUnexpected(Error{kind, std::move(reason)});
}

// One AUTHORIZATION_TOKEN parameter, copied out of the message.
struct AuthToken {
  uint64_t tokenType{0};
  std::string value;
};

// Read access to a message's parameters. Nothing is copied until asked for.
class ParamsView {
public:
  // `params` is a moxygen::Parameters; the host builds views, plugins read them.
  explicit ParamsView(const void* params) : params_(params) {}

  std::vector<AuthToken> authTokens(uint64_t tokenType) const;
  std::optional<uint64_t> uintParam(uint64_t key) const;
  std::optional<std::string_view> bytesParam(uint64_t key) const;

private:
  const void* params_;
};

struct SessionView {
  std::string_view service;
  std::string_view authority;
  std::string_view path;
  uint64_t version{0};
};

struct SetupView {
  const SessionView& session;
  ParamsView params;
};

enum class RequestKind {
  Subscribe,
  Fetch,
  TrackStatus,
  Publish,
  PublishNamespace,
  SubscribeNamespace,
  SubscribeTracks,
};

struct RequestView {
  RequestKind kind;
  // The prefix for SubscribeNamespace and SubscribeTracks.
  std::span<const std::string> ns;
  // Unset for namespace-level requests.
  std::optional<std::string_view> track;
  ParamsView params;
  const SessionView& session;
  // The request carries the relay peering token and peering is enabled.
  bool peer{false};
};

// MoQT key-value rules: an even key carries a number, an odd key bytes. Keys
// the host or moxygen writes itself are refused.
struct SetupParam {
  uint64_t key{0};
  std::variant<uint64_t, std::string> value;
};
using Params = std::vector<SetupParam>;

// A configured ClientHello policy. Values may depend on it, never on the
// connection: they are computed once at startup.
struct ListenerScope {
  std::string_view listener;
};

// One connection attempt to an upstream of `service`.
struct UpstreamScope {
  std::string_view service;
  std::string_view url;
};

// What `admit` returns; the plugin downcasts its own type in later hooks.
class SessionState {
public:
  virtual ~SessionState() = default;
};

enum class Hook : uint32_t {
  Admit = 1U << 0,
  SessionEnded = 1U << 1,
  Authorize = 1U << 2,
  UpstreamSetupParams = 1U << 3,
  StateSnapshot = 1U << 4,
};

class HookSet {
public:
  constexpr HookSet() = default;
  constexpr HookSet(Hook hook) : bits_(static_cast<uint32_t>(hook)) {} // NOLINT
  constexpr HookSet operator|(HookSet other) const { return HookSet(bits_ | other.bits_); }
  constexpr bool has(Hook hook) const { return (bits_ & static_cast<uint32_t>(hook)) != 0; }

private:
  constexpr explicit HookSet(uint32_t bits) : bits_(bits) {}
  uint32_t bits_{0};
};

constexpr HookSet operator|(Hook a, Hook b) {
  return HookSet(a) | HookSet(b);
}

// A plugin's view of one service.
// - Session and request hooks run on the session's I/O worker, concurrently
//   across workers.
// - start, stop and stateSnapshot run on the service's relay executor.
// - The host calls only the hooks named by hooks().
class ServiceInstance {
public:
  virtual ~ServiceInstance() = default;

  virtual HookSet hooks() const = 0;

  // In config order before the service accepts sessions; stop() in reverse.
  virtual folly::coro::Task<void> start() { co_return; }
  virtual folly::coro::Task<void> stop() { co_return; }

  // An empty state is valid.
  virtual folly::coro::Task<Result<std::shared_ptr<SessionState>>> admit(const SetupView& setup);

  // Once per session whose admit succeeded on this plugin, after every check
  // on the session has finished; also when a later plugin rejected it.
  virtual folly::coro::Task<void> sessionEnded(std::shared_ptr<SessionState> state);

  // `state` is what this plugin's admit returned, or null.
  virtual folly::coro::Task<Result<folly::Unit>>
  authorize(const RequestView& request, const SessionState* state);

  virtual folly::coro::Task<Result<Params>> setupParams(const UpstreamScope& scope);

  // Shown in /state under services.<service>.plugins.<id>.
  virtual folly::coro::Task<folly::dynamic> stateSnapshot();
};

struct ServiceContext {
  std::string_view serviceName;
  // Where start, stop and stateSnapshot run.
  folly::Executor::KeepAlive<> relayExecutor;
};

// One per `plugins:` entry, created by the registered factory.
// - start and stop run on the main EventBase, in config order and reverse.
// - start runs under a cancellation the host fires at shutdown.
class Root {
public:
  virtual ~Root() = default;

  virtual folly::coro::Task<void> start() { co_return; }
  virtual folly::coro::Task<void> stop() { co_return; }

  // Process-scope plugins only. Called once per listener after start().
  virtual bool declaresListenerParams() const { return false; }
  virtual folly::coro::Task<Result<Params>> setupParams(const ListenerScope& scope);

  virtual Result<std::unique_ptr<ServiceInstance>> createService(const ServiceContext& ctx) = 0;
};

} // namespace openmoq::moqx::plugin
