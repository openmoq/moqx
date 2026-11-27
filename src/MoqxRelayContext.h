/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <string_view>

#include "MoqxRelay.h"
#include "ServiceMatcher.h"
#include "UpstreamProvider.h"
#include "auth/Auth.h"
#include "config/Config.h"
#include "stats/MoQStatsCollector.h"
#include "stats/StatsRegistry.h"
#include <moxygen/MoQExtension.h>
#include <moxygen/MoQServerBase.h>
#include <moxygen/MoQSession.h>

#include <folly/Expected.h>
#include <folly/ThreadLocal.h>
#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <folly/coro/Task.h>
#include <folly/executors/IOThreadPoolExecutor.h>
#include <folly/io/async/EventBase.h>

#include <vector>

#include <optional>

namespace openmoq::moqx {

namespace logging {
class AccessLog;
class AccessLogSessionContext;
} // namespace logging

// Returns nullptr to use the system CAs. Throws if tls.ca_cert cannot be loaded.
std::shared_ptr<fizz::CertificateVerifier> makeUpstreamVerifier(const config::UpstreamTlsConfig& tls
);

// One verifier per upstream, in MoqtRelayBackend::upstreamConfigs order. Never throws;
// the error names the failing upstream. A null element means "system CAs".
folly::Expected<UpstreamVerifiers, std::string>
buildUpstreamVerifiers(const config::ServiceConfig& svc);

// Visitor interface for the top-level relay context state dump.
// onServiceBegin returns the RelayStateVisitor to use for that service's relay.
class RelayContextVisitor {
public:
  virtual ~RelayContextVisitor() = default;
  virtual void onRelayBegin(std::string_view relayID, int64_t activeSessions) = 0;
  // Called before relay->dumpState(). Returns the visitor to pass into dumpState.
  virtual RelayStateVisitor& onServiceBegin(std::string_view name) = 0;
  struct UpstreamState {
    std::string url;
    std::string state;
  };
  // Called after relay->dumpState() if an upstream is configured for this service.
  virtual void onServiceUpstream(std::string_view url, std::string_view state) = 0;
  // As onServiceUpstream, for a service configured with an upstreams list.
  virtual void onServiceUpstreams(const std::vector<UpstreamState>& upstreams) = 0;
  virtual void onServiceEnd() = 0;
  virtual void onRelayEnd() = 0;
  // Checked between services so a consumer that has gone away stops the walk
  // rather than paying for every remaining service.
  virtual bool alive() const = 0;
};

// Holds relay state shared across all listener instances.
// A single MoqxRelayContext is shared by every MoqxRelayServer so that
// publishers and subscribers connecting on different listeners can exchange
// data through the same MoqxRelay instances.
class MoqxRelayContext {
public:
  struct ServiceEntry {
    config::ServiceConfig config;
    // Declared before relay, so the relay is destroyed first.
    std::unique_ptr<PeeringBackend> peering;
    std::shared_ptr<MoqxRelay> relay;
    std::shared_ptr<const auth::AuthTokenVerifier> verifier;
  };

  // Matches services bound to one listener.
  // The context must outlive the matcher.
  // Reads the calling thread's service view, so a thread that has not had a view
  // installed matches nothing. Use the result before the view is swapped.
  class ListenerMatcher {
  public:
    const std::string& name() const { return name_; }

    // Matches only the services bound to this listener.
    std::optional<std::string_view> match(std::string_view authority, std::string_view path) const;

    // Unique exact paths of the services bound to this listener.
    std::vector<std::string> exactPaths() const;

  private:
    friend class MoqxRelayContext;
    ListenerMatcher(std::string name, const MoqxRelayContext* ctx)
        : name_(std::move(name)), ctx_(ctx) {}

    // The calling thread's matcher for this listener, or null.
    const ServiceMatcher* current() const;

    std::string name_;
    const MoqxRelayContext* ctx_;
  };

  MoqxRelayContext(
      const folly::F14FastMap<std::string, config::ServiceConfig>& services,
      const std::string& relayID,
      bool useRelayThread = true,
      bool useLocalForwarders = false,
      std::optional<config::ClusterConfig> cluster = config::ClusterConfig{}
  );

  // If listenerName is unknown or no service is bound to it,
  // the matcher does not match any service.
  ListenerMatcher listenerMatcher(std::string_view listenerName) const;

  void setStatsRegistry(std::shared_ptr<stats::StatsRegistry> registry);

  // The HOP_ID to declare in SETUP. Unset when MoQ Cluster is disabled.
  std::optional<uint64_t> getRelayHopID() const {
    return cluster_ ? std::optional<uint64_t>(cluster_->hopID) : std::nullopt;
  }

  // The extensions every listener adds: MoQ Cluster when it is enabled, and Top
  // Tracks so subscribers may send TOP_TRACKS_FILTER.
  std::vector<std::shared_ptr<moxygen::MoQExtension>> makeServerExtensions() const;

  // Starts each service's peering backend. Call once, before the servers start accepting.
  // The backends may use workerEvb for anything that needs an EventBase.
  void initUpstreams(folly::EventBase* workerEvb);

  // False until initUpstreams() runs, before which there is nothing to walk.
  // False until the service view is installed for the current thread.
  bool ready() const { return workerEvb_ != nullptr && *tlServiceView_ != nullptr; }

  // Walks every service, calling visitor methods as it goes. Each service's
  // walk is placed on the executor that owns that service's state; the visitor
  // is called from there for the walk itself and from the caller's executor for
  // everything around it, one service at a time. Awaitable from any executor.
  folly::coro::Task<void> dumpState(RelayContextVisitor& visitor) const;

  // Signals all relay upstreams to stop. Call before destroying servers so
  // reconnect coroutines can exit before worker EVBs are drained.
  void stop();

  // Runs a barrier across ioEvbs and every relay executor, waiting for each
  // round to finish everywhere before posting the next.
  // This is an expedient fixup to ensure the threads live long enough without holding
  // KeepAlive tokens throughout.
  void drainExecs(const std::vector<folly::Executor::KeepAlive<folly::EventBase>>& ioEvbs);

  // Stops the peering backends' own threads. Call after drainExecs().
  void join();

  enum class PurgeError { ServiceRequired, UnknownService };

  // Force-evicts one service's tracks matching ftn or ns, or all of them when
  // both are empty, and returns the count. An empty serviceName selects the
  // only service. Awaitable from any executor.
  folly::coro::Task<folly::Expected<size_t, PurgeError>> purgeCache(
      std::string_view serviceName,
      std::optional<moxygen::FullTrackName> ftn = {},
      std::optional<moxygen::TrackNamespace> ns = {}
  );

  struct TrackMetricsEntry {
    std::string service;
    moxygen::FullTrackName ftn;
    stats::TrackCounters counters;
  };

  struct TrackMetricsResult {
    std::vector<TrackMetricsEntry> tracks;
    // Total matches across services, before the limit; tracks is empty when
    // this exceeds the limit so the caller can reject rather than truncate.
    size_t matched{0};
  };

  // serviceName empty matches every service; trackName null matches the whole
  // namespace prefix.
  folly::coro::Task<TrackMetricsResult> aggregateTrackMetrics(
      std::string serviceName,
      moxygen::TrackNamespace nsPrefix,
      std::optional<std::string> trackName,
      size_t limit
  ) const;

  // --- Delegation targets for MoqxRelayServer virtual overrides ---

  void onNewSession(const ListenerMatcher& listener, std::shared_ptr<moxygen::MoQSession> session);
  // Ends the session on the service recorded at SETUP, looked up by name in the calling
  // thread's service view. Call on the session's executor.
  void onSessionEnd(std::shared_ptr<moxygen::MoQSession> session);
  // The server rejected the setup before validateAuthority ran.
  void onSetupRejected(
      const ListenerMatcher& listener,
      const std::shared_ptr<moxygen::MoQSession>& session,
      moxygen::SessionCloseErrorCode error
  );

  // Must run before any listener accepts sessions.
  void setAccessLog(std::shared_ptr<logging::AccessLog> accessLog);

  // Must run after setStatsRegistry and before any listener accepts sessions.
  // initTrackStats=false skips per-track counting entirely, leaving the
  // data-path filters uninstalled rather than installed-and-ignored.
  void
  initThreadStatsCollectors(folly::IOThreadPoolExecutor& ioExecutor, bool initTrackStats = true);

  // listener: the one the session arrived on. Only services bound to it can match.
  folly::Expected<folly::Unit, moxygen::SessionCloseErrorCode> validateAuthority(
      const ListenerMatcher& listener,
      const moxygen::ClientSetup& clientSetup,
      uint64_t negotiatedVersion,
      std::shared_ptr<moxygen::MoQSession> session
  );

  // Gives each target thread its own service view, swapped in on that thread so the
  // old view is destroyed where it was used. Blocks until every swap has run, so call
  // it only when no target needs the calling thread (startup, tests).
  // Targets are the I/O and admin EventBases; do not include the constructing thread's.
  void installServiceViews(const std::vector<folly::Executor::KeepAlive<folly::EventBase>>& evbs);

private:
  folly::Expected<folly::Unit, moxygen::SessionCloseErrorCode> authorizeSession(
      const ListenerMatcher& listener,
      const moxygen::ClientSetup& clientSetup,
      const std::shared_ptr<moxygen::MoQSession>& session,
      std::optional<std::string_view> matchedName,
      std::shared_ptr<logging::AccessLogSessionContext> accessLogContext
  );

  // Startup only: passes no verifiers, so a bad CA throws from initUpstreams(), which main
  // catches. Throwing here would escape the constructor.
  void addService(const std::string& name, const config::ServiceConfig& svc, folly::EventBase* evb);

  // evb is the relay's own thread, or null in single-thread mode. Local forwarders need a
  // relay EventBase; without one they stay off.
  std::shared_ptr<MoqxRelay>
  makeRelay(const config::ServiceConfig& svc, folly::EventBase* evb) const;

  // Builds a service without inserting it or touching any thread's view; the entry's address
  // is fixed from here on. Does not call peering->start(), so an entry that was never started
  // can be discarded. verifiers: prebuilt (reload), or nullopt to have start() build them
  // (startup).
  std::unique_ptr<ServiceEntry> prepareService(
      const config::ServiceConfig& svc,
      folly::EventBase* evb,
      std::optional<UpstreamVerifiers> verifiers
  ) const;

  struct ThreadServiceView {
    folly::F14NodeMap<std::string, ServiceMatcher> listenerMatchers;
    folly::F14FastMap<std::string, ServiceEntry*> services;
  };

  std::unique_ptr<ThreadServiceView> buildView() const;

  // When use_relay_thread=true: one dedicated thread per service, each with its
  // own executor, isolating relay state from I/O threads.  Null when disabled.
  // Each relay owns its MoQFollyExecutorImpl; the pool just keeps threads alive.
  std::unique_ptr<folly::IOThreadPoolExecutor> relayThreadPool_;

  folly::F14FastMap<std::string, std::unique_ptr<ServiceEntry>> services_;
  std::string relayID_;
  std::optional<config::ClusterConfig> cluster_;
  bool useRelayThread_;
  bool useLocalForwarders_;
  std::shared_ptr<stats::StatsRegistry> statsRegistry_;
  // One collector per io thread; tlStatsCollector_ binds each to its own thread.
  std::vector<std::shared_ptr<stats::MoQStatsCollector>> statsCollectors_;
  folly::ThreadLocal<std::shared_ptr<stats::MoQStatsCollector>> tlStatsCollector_;
  folly::ThreadLocal<std::unique_ptr<ThreadServiceView>> tlServiceView_;
  folly::EventBase* workerEvb_{nullptr};
  std::shared_ptr<logging::AccessLog> accessLog_;
};

} // namespace openmoq::moqx
