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
#include "config/Config.h"
#include "stats/MoQStatsCollector.h"
#include "stats/StatsRegistry.h"
#include <moxygen/MoQServerBase.h>
#include <moxygen/MoQSession.h>

#include <folly/Expected.h>
#include <folly/container/F14Map.h>
#include <folly/coro/Task.h>
#include <folly/io/async/EventBase.h>

#include <optional>

namespace openmoq::moqx {

// Visitor interface for the top-level relay context state dump.
// onServiceBegin returns the RelayStateVisitor to use for that service's relay.
class RelayContextVisitor {
public:
  virtual ~RelayContextVisitor() = default;
  virtual void onRelayBegin(std::string_view relayID, int64_t activeSessions) = 0;
  // Called before relay->dumpState(). Returns the visitor to pass into dumpState.
  virtual RelayStateVisitor& onServiceBegin(std::string_view name) = 0;
  // Called after relay->dumpState() if an upstream is configured for this service.
  virtual void onServiceUpstream(std::string_view url, std::string_view state) = 0;
  virtual void onServiceEnd() = 0;
  virtual void onRelayEnd() = 0;
};

// Holds relay state shared across all listener instances.
// A single MoqxRelayContext is shared by every MoqxRelayServer so that
// publishers and subscribers connecting on different listeners can exchange
// data through the same MoqxRelay instances.
class MoqxRelayContext {
public:
  struct ServiceEntry {
    config::ServiceConfig config;
    std::shared_ptr<MoqxRelay> relay;
  };

  MoqxRelayContext(
      const folly::F14FastMap<std::string, config::ServiceConfig>& services,
      const std::string& relayID
  );

  void setStatsRegistry(std::shared_ptr<stats::StatsRegistry> registry);

  // Must be called once, after all MoqxRelayServer instances have been started,
  // so that worker EVBs are available. workerEvb is used for upstream connections.
  void initUpstreams(folly::EventBase* workerEvb);

  // Sets the cache EVB used to serialize purge() calls with relay callbacks.
  void setCacheEvb(folly::EventBase* evb) { cacheEvb_ = evb; }

  // Returns the worker EVB used for upstream connections.
  // Null until initUpstreams() is called.
  folly::EventBase* workerEvb() const { return workerEvb_; }

  // Returns the cache EVB used to serialize purge() calls with relay callbacks.
  // Null until setCacheEvb() is called.
  folly::EventBase* cacheEvb() const { return cacheEvb_; }

  // Dumps a snapshot of relay state by calling visitor methods.
  // MUST be called on workerEvb() to avoid data races.
  void dumpState(RelayContextVisitor& visitor) const;

  // Signals all relay upstreams to stop. Call before destroying servers so
  // reconnect coroutines can exit before worker EVBs are drained.
  void stop();

  // Force-evicts cached tracks unconditionally. Scoped by optional ftn or ns;
  // if both are empty all tracks are evicted. Optionally scoped to a single
  // service. Returns number of tracks evicted.
  // MUST be awaited on cacheEvb().
  folly::coro::Task<size_t> purgeCache(
      std::string_view serviceName = {},
      std::optional<moxygen::FullTrackName> ftn = {},
      std::optional<moxygen::TrackNamespace> ns = {}
  );

  // Returns the unique set of exact paths registered across all services.
  // Used by pico listeners to populate the h3zero WebTransport path table.
  std::vector<std::string> getExactServicePaths() const;

  // --- Delegation targets for MoqxRelayServer virtual overrides ---

  void onNewSession(std::shared_ptr<moxygen::MoQSession> session);
  void onSessionEnd();

  folly::Expected<folly::Unit, moxygen::SessionCloseErrorCode> validateAuthority(
      const moxygen::ClientSetup& clientSetup,
      uint64_t negotiatedVersion,
      std::shared_ptr<moxygen::MoQSession> session
  );

private:
  folly::F14FastMap<std::string, ServiceEntry> services_;
  ServiceMatcher serviceMatcher_;
  std::string relayID_;
  folly::EventBase* cacheEvb_{nullptr};
  std::shared_ptr<stats::StatsRegistry> statsRegistry_;
  std::shared_ptr<stats::MoQStatsCollector> statsCollector_;
  folly::EventBase* workerEvb_{nullptr};
};

} // namespace openmoq::moqx
