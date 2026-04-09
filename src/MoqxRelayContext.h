#pragma once

#include <memory>

#include "MoqxRelay.h"
#include "ServiceMatcher.h"
#include "UpstreamProvider.h"
#include "config/config.h"
#include "stats/MoQStatsCollector.h"
#include "stats/StatsRegistry.h"
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

  // Signals all relay upstreams to stop. Call before destroying servers so
  // reconnect coroutines can exit before worker EVBs are drained.
  void stop();

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
  std::shared_ptr<stats::StatsRegistry> statsRegistry_;
  std::shared_ptr<stats::MoQStatsCollector> statsCollector_;
};

} // namespace openmoq::moqx
