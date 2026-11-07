/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// The host side of the plugin API: roots, service instances, chains, the
// per-session record and lifecycle.
// - Chains: process entries, then service entries, each by rank, ties in list
//   order. Lifecycle runs in config order instead.
// - Hook calls of a session run on its executor; nothing here locks for them.
// - stop() waits for every session record and in-flight hook to finish.

#include "config/Config.h"
#include "plugin/Api.h"
#include "plugin/PluginType.h"

#include <folly/CancellationToken.h>
#include <folly/Executor.h>
#include <folly/Expected.h>
#include <folly/coro/Task.h>
#include <folly/futures/Future.h>
#include <folly/json/dynamic.h>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace openmoq::moqx::plugin {

// Counts work that stop() must wait for.
class InflightTracker {
public:
  // False once closed: the caller must not start the work.
  bool enter();
  void exit();
  // Refuses new work; drained() completes once the count reaches zero.
  void close();
  folly::coro::Task<void> drained();

private:
  std::mutex mutex_;
  size_t count_{0};
  bool closed_{false};
  std::vector<folly::Promise<folly::Unit>> waiters_;
};

// Holds one unit of tracked work, if enter() allowed it.
class InflightGuard {
public:
  InflightGuard() = default;
  explicit InflightGuard(std::shared_ptr<InflightTracker> tracker);
  InflightGuard(InflightGuard&& other) noexcept : tracker_(std::move(other.tracker_)) {}
  InflightGuard& operator=(InflightGuard&& other) noexcept;
  InflightGuard(const InflightGuard&) = delete;
  InflightGuard& operator=(const InflightGuard&) = delete;
  ~InflightGuard();

  explicit operator bool() const { return tracker_ != nullptr; }

private:
  std::shared_ptr<InflightTracker> tracker_;
};

struct ChainMember {
  std::string id;
  std::shared_ptr<ServiceInstance> instance;
  HookSet hooks;
};

class ServiceChain {
public:
  ServiceChain(
      std::string service,
      std::vector<ChainMember> members,
      std::chrono::milliseconds admissionTimeout,
      std::chrono::milliseconds authorizeTimeout,
      folly::Executor::KeepAlive<> relayExecutor
  );

  const std::string& service() const { return service_; }
  // In chain order.
  const std::vector<ChainMember>& members() const { return members_; }
  bool any(Hook hook) const;
  std::chrono::milliseconds admissionTimeout() const { return admissionTimeout_; }
  std::chrono::milliseconds authorizeTimeout() const { return authorizeTimeout_; }
  const folly::Executor::KeepAlive<>& relayExecutor() const { return relayExecutor_; }

private:
  std::string service_;
  std::vector<ChainMember> members_;
  std::chrono::milliseconds admissionTimeout_;
  std::chrono::milliseconds authorizeTimeout_;
  folly::Executor::KeepAlive<> relayExecutor_;
};

// What a request check needs. `keepAlive` owns the message the views borrow,
// so a check that outlives its budget still reads valid memory.
struct RequestArgs {
  RequestKind kind{RequestKind::Subscribe};
  std::span<const std::string> ns;
  std::optional<std::string_view> track;
  const void* params{nullptr};
  bool peer{false};
  std::shared_ptr<const void> keepAlive;
};

// One session's plugin state: what each plugin's admit returned.
// - Created before admission; its checks run on `executor`.
// - end() runs sessionEnded after the session's checks have finished.
class PluginSession : public std::enable_shared_from_this<PluginSession> {
public:
  PluginSession(
      std::shared_ptr<const ServiceChain> chain,
      std::string authority,
      std::string path,
      uint64_t version,
      folly::Executor::KeepAlive<> executor,
      InflightGuard hostGuard
  );

  const ServiceChain& chain() const { return *chain_; }
  const SessionView& view() const { return view_; }

  // The admit chain within the admission budget. `params` points into
  // `keepAlive`.
  folly::coro::Task<Result<folly::Unit>>
  admit(const void* params, std::shared_ptr<const void> keepAlive);

  struct StartedCheck {
    // Ready on return from startAuthorize when no hook suspended.
    folly::SemiFuture<Result<folly::Unit>> result{folly::SemiFuture<Result<folly::Unit>>::makeEmpty(
    )};
    // Cancels the check when its budget runs out.
    folly::CancellationSource timeout;
  };

  // Starts the authorize chain inline on the session executor. The budget is
  // not applied here.
  StartedCheck startAuthorize(RequestArgs args, folly::CancellationToken requestToken);

  // startAuthorize, awaited within the authorize budget.
  folly::coro::Task<Result<folly::Unit>>
  authorize(RequestArgs args, folly::CancellationToken requestToken);

  // Applies the authorize budget to a started check.
  folly::coro::Task<Result<folly::Unit>> awaitAuthorize(StartedCheck check);

  // Session over: cancels checks, then runs sessionEnded once they finish.
  void end();
  bool ended() const { return ended_; }

  const folly::Executor::KeepAlive<>& executor() const { return executor_; }

private:
  // Lazy tasks: `self` is taken when the task is made, not when it starts.
  folly::coro::Task<Result<folly::Unit>> runAdmit(
      std::shared_ptr<PluginSession> self,
      const void* params,
      std::shared_ptr<const void> keepAlive,
      InflightGuard guard
  );
  folly::coro::Task<Result<folly::Unit>>
  runAuthorize(std::shared_ptr<PluginSession> self, RequestArgs args, InflightGuard guard);
  folly::coro::Task<void> runSessionEnded(std::shared_ptr<PluginSession> self);

  std::shared_ptr<const ServiceChain> chain_;
  std::string authority_;
  std::string path_;
  SessionView view_;
  folly::Executor::KeepAlive<> executor_;
  InflightGuard hostGuard_;
  // Checks of this session; sessionEnded waits for them.
  std::shared_ptr<InflightTracker> checks_;
  folly::CancellationSource cancel_;
  // Indexed like chain().members(); set when that plugin's admit succeeded.
  std::vector<std::optional<std::shared_ptr<SessionState>>> states_;
  bool ended_{false};
};

struct PluginSnapshot {
  std::string id;
  // The snapshot as JSON; empty when `error` is set.
  std::string json;
  // Set when the snapshot failed, stalled or could not be written as JSON.
  std::optional<std::string> error;
};

class PluginHost {
public:
  // Runs every factory. Roots are ordered: process entries, then each
  // service's entries by service name.
  static folly::Expected<std::shared_ptr<PluginHost>, std::string>
  create(const config::Config& config);

  // On the calling executor, in config order. A failure stops what started,
  // in reverse, and returns the error.
  folly::coro::Task<folly::Expected<folly::Unit, std::string>>
  startRoots(folly::CancellationToken token);

  // Listener SETUP parameters from process roots. Duplicate or reserved keys
  // are errors.
  folly::coro::Task<folly::Expected<Params, std::string>> listenerParams(std::string_view listener);

  using ExecutorFor = std::function<folly::Executor::KeepAlive<>(std::string_view service)>;

  // After the relays exist; `executorFor` names each service's relay executor.
  folly::Expected<folly::Unit, std::string> createServices(const ExecutorFor& executorFor);

  // Each on its relay executor, services by name, instances in config order.
  // A failure stops what started, in reverse.
  folly::coro::Task<folly::Expected<folly::Unit, std::string>>
  startServices(folly::CancellationToken token);

  // Null for a service with no plugins.
  std::shared_ptr<const ServiceChain> chain(std::string_view service) const;

  // Null once stop() has begun; the session must then be refused.
  std::shared_ptr<PluginSession> newSession(
      std::string_view service,
      std::string authority,
      std::string path,
      uint64_t version,
      folly::Executor::KeepAlive<> executor
  );

  // Upstream SETUP parameters of `service`'s chain for one connect.
  folly::coro::Task<folly::Expected<Params, std::string>>
  upstreamParams(std::string service, std::string url);

  // On the relay executor; a failure or a stall past `budget` is reported per plugin.
  folly::coro::Task<std::vector<PluginSnapshot>>
  snapshots(std::string service, std::chrono::milliseconds budget);

  // Waits for sessions and hook calls, then stops services and roots in
  // reverse order. Errors are logged; every stop() still runs.
  folly::coro::Task<void> stop();

private:
  struct RootEntry {
    config::PluginEntryConfig config;
    plugin::Scope scope;
    // Empty for process scope.
    std::string service;
    std::shared_ptr<Root> root;
    bool started{false};
  };
  struct ServiceEntry {
    std::string service;
    std::shared_ptr<const ServiceChain> chain;
    // Config order, for lifecycle.
    std::vector<std::shared_ptr<ServiceInstance>> lifecycle;
    std::vector<std::string> lifecycleIds;
    size_t started{0};
  };

  folly::coro::Task<void> stopServices();
  folly::coro::Task<void> stopRoots();

  std::map<std::string, std::vector<config::PluginEntryConfig>> serviceConfigs_;
  std::map<std::string, std::pair<std::chrono::milliseconds, std::chrono::milliseconds>> timeouts_;
  std::vector<RootEntry> roots_;
  std::map<std::string, ServiceEntry, std::less<>> services_;
  std::shared_ptr<InflightTracker> tracker_{std::make_shared<InflightTracker>()};
};

// Merges one plugin's parameters into `into`; a key already present or
// written by the host is an error naming `owner`.
folly::Expected<folly::Unit, std::string>
mergeParams(Params& into, const Params& from, std::string_view owner);

} // namespace openmoq::moqx::plugin
