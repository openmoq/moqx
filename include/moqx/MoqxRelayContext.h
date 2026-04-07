#pragma once

#include <memory>
#include <string_view>

#include <moqx/MoqxRelay.h>
#include <moqx/ServiceMatcher.h>
#include <moqx/UpstreamProvider.h>
#include <moqx/config/config.h>
#include <moqx/stats/MoQStatsCollector.h>
#include <moqx/stats/StatsRegistry.h>
#include <moxygen/MoQServerBase.h>
#include <moxygen/MoQSession.h>

#include <folly/Expected.h>
#include <folly/container/F14Map.h>
#include <folly/io/async/EventBase.h>

namespace openmoq::moqx {

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

  // Sets the worker EVB used to serialize clearCaches() calls with relay
  // callbacks.
  void setWorkerEvb(folly::EventBase* evb) { workerEvb_ = evb; }

  // Signals all relay upstreams to stop. Call before destroying servers so
  // reconnect coroutines can exit before worker EVBs are drained.
  void stop();

  // Clears relay caches, serialized onto the worker EVB.
  // Pass an empty serviceName to clear all services.
  // Returns the number of caches cleared (0 if a named service was not found).
  size_t clearCaches(std::string_view serviceName = {});

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
  folly::EventBase* workerEvb_{nullptr};
  std::shared_ptr<stats::StatsRegistry> statsRegistry_;
  std::shared_ptr<stats::MoQStatsCollector> statsCollector_;
};

} // namespace openmoq::moqx
