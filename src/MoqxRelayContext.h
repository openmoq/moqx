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
#include "peering/MoqPeeringRuntime.h"
#include "stats/MoQStatsCollector.h"
#include "stats/StatsRegistry.h"
#include <moxygen/MoQExtension.h>
#include <moxygen/MoQServerBase.h>
#include <moxygen/MoQSession.h>

#include <folly/Expected.h>
#include <folly/ThreadLocal.h>
#include <folly/container/F14Map.h>
#include <folly/coro/Task.h>
#include <folly/executors/IOThreadPoolExecutor.h>
#include <folly/io/async/EventBase.h>

#include <vector>

#include <optional>

namespace openmoq::moqx {

class MoqPeeringCore;

// Returns nullptr to use the system CAs. Throws if tls.ca_cert cannot be loaded.
std::shared_ptr<fizz::CertificateVerifier>
makeUpstreamVerifier(const config::UpstreamTlsConfig& tls);

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

  // moqPeering is set when the process selected moq-peering. Each service then uses
  // that backend instead of MoqtRelayBackend. Null selects MoQT relay peering.
  MoqxRelayContext(
      const folly::F14FastMap<std::string, config::ServiceConfig>& services,
      const std::string& relayID,
      bool useRelayThread = true,
      bool useLocalForwarders = false,
      std::optional<config::ClusterConfig> cluster = config::ClusterConfig{},
      std::shared_ptr<peering::MoqPeeringRuntime> moqPeering = nullptr
  );
  ~MoqxRelayContext();

  void setStatsRegistry(std::shared_ptr<stats::StatsRegistry> registry);

  // The local moq-peering node, when this process selected that method and the
  // runtime is still alive.
  std::optional<peering::PeeringStatus> moqPeeringStatus() const;

  // Runs the op on the moq-peering backend. Cluster peering answers Unsupported.
  folly::coro::Task<PeerAdminResult> executePeerAdminOp(PeerAdminOp op);

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
  bool ready() const { return workerEvb_ != nullptr; }

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

  // Returns the unique set of exact paths registered across all services.
  // Used by pico listeners to populate the h3zero WebTransport path table.
  std::vector<std::string> getExactServicePaths() const;

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

  void onNewSession(std::shared_ptr<moxygen::MoQSession> session);
  void onSessionEnd(std::shared_ptr<moxygen::MoQSession> session);

  // Must run after setStatsRegistry and before any listener accepts sessions.
  // initTrackStats=false skips per-track counting entirely, leaving the
  // data-path filters uninstalled rather than installed-and-ignored.
  void
  initThreadStatsCollectors(folly::IOThreadPoolExecutor& ioExecutor, bool initTrackStats = true);

  folly::Expected<folly::Unit, moxygen::SessionCloseErrorCode> validateAuthority(
      const moxygen::ClientSetup& clientSetup,
      uint64_t negotiatedVersion,
      std::shared_ptr<moxygen::MoQSession> session
  );

private:
  void addService(
      const std::string& name,
      const config::ServiceConfig& svc,
      std::shared_ptr<MoqxRelay> relay
  );

  // When use_relay_thread=true: one dedicated thread per service, each with its
  // own executor, isolating relay state from I/O threads.  Null when disabled.
  // Each relay owns its MoQFollyExecutorImpl; the pool just keeps threads alive.
  std::unique_ptr<folly::IOThreadPoolExecutor> relayThreadPool_;

  folly::F14FastMap<std::string, ServiceEntry> services_;
  ServiceMatcher serviceMatcher_;
  std::string relayID_;
  std::optional<config::ClusterConfig> cluster_;
  std::shared_ptr<stats::StatsRegistry> statsRegistry_;
  // One collector per io thread; tlStatsCollector_ binds each to its own thread.
  std::vector<std::shared_ptr<stats::MoQStatsCollector>> statsCollectors_;
  folly::ThreadLocal<std::shared_ptr<stats::MoQStatsCollector>> tlStatsCollector_;
  folly::EventBase* workerEvb_{nullptr};
  // Process-wide moq-peering state. Null when the method is cluster.
  // The destructor disarms it while the relays in services_ are still alive.
  std::shared_ptr<MoqPeeringCore> moqPeering_;
};

} // namespace openmoq::moqx
