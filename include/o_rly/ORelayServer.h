#pragma once

#include <memory>

#include <moxygen/MoQServer.h>
#include <o_rly/ORelay.h>
#include <o_rly/ServiceMatcher.h>
#include <o_rly/UpstreamProvider.h>
#include <o_rly/config/config.h>
#include <o_rly/stats/MoQStatsCollector.h>
#include <o_rly/stats/StatsRegistry.h>

namespace openmoq::o_rly {

class ORelayServer : public moxygen::MoQServer {
public:
  // Used when the insecure flag is false
  ORelayServer(
      const std::string& cert,
      const std::string& key,
      const std::string& endpoint,
      const std::string& versions,
      folly::F14FastMap<std::string, config::ServiceConfig> services,
      const std::string& relayID = {}
  );

  // Used when the insecure flag is true
  ORelayServer(
      const std::string& endpoint,
      const std::string& versions,
      folly::F14FastMap<std::string, config::ServiceConfig> services,
      const std::string& relayID = {}
  );

  void setStatsRegistry(std::shared_ptr<stats::StatsRegistry> registry);

  // Binds listeners then initialises per-service upstream providers (requires
  // draft 16+ for relay chaining).
  void start(const folly::SocketAddress& addr) {
    MoQServer::start(addr);
    initUpstreams();
  }

  void onNewSession(std::shared_ptr<moxygen::MoQSession> clientSession) override;

  void terminateClientSession(std::shared_ptr<moxygen::MoQSession> session) override;

  folly::Expected<folly::Unit, moxygen::SessionCloseErrorCode> validateAuthority(
      const moxygen::ClientSetup& clientSetup,
      uint64_t negotiatedVersion,
      std::shared_ptr<moxygen::MoQSession> session
  ) override;

protected:
  std::shared_ptr<moxygen::MoQSession> createSession(
      folly::MaybeManagedPtr<proxygen::WebTransport> wt,
      std::shared_ptr<moxygen::MoQExecutor> executor
  ) override;

private:
  void initServices(
      const folly::F14FastMap<std::string, config::ServiceConfig>& services,
      const std::string& relayID
  );
  void initUpstreams();

  struct ServiceEntry {
    config::ServiceConfig config;
    std::shared_ptr<ORelay> relay;
  };
  folly::F14FastMap<std::string, ServiceEntry> services_;
  ServiceMatcher serviceMatcher_;
  std::string relayID_;
  std::shared_ptr<stats::StatsRegistry> statsRegistry_;
  std::shared_ptr<stats::MoQStatsCollector> statsCollector_;
};

} // namespace openmoq::o_rly
