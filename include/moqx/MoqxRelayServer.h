#pragma once

#include <memory>

#include <moqx/MoqxRelay.h>
#include <moqx/ServiceMatcher.h>
#include <moqx/UpstreamProvider.h>
#include <moqx/config/config.h>
#include <moqx/stats/MoQStatsCollector.h>
#include <moqx/stats/StatsRegistry.h>
#include <moxygen/MoQServer.h>

namespace openmoq::moqx {

class MoqxRelayServer : public moxygen::MoQServer {
public:
  // Used when the insecure flag is false
  MoqxRelayServer(
      const std::string& cert,
      const std::string& key,
      const std::string& endpoint,
      const std::string& versions,
      folly::F14FastMap<std::string, config::ServiceConfig> services,
      const std::string& relayID = {}
  );

  // Used when the insecure flag is true
  MoqxRelayServer(
      const std::string& endpoint,
      const std::string& versions,
      folly::F14FastMap<std::string, config::ServiceConfig> services,
      const std::string& relayID = {}
  );

  ~MoqxRelayServer() override;

  void setStatsRegistry(std::shared_ptr<stats::StatsRegistry> registry);

  // Clears the cache for all services, or for a single named service 
  // Returns the number of caches cleared (0 means the named service was not found).
  size_t clearCaches(std::string_view serviceName = {});

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
    std::shared_ptr<MoqxRelay> relay;
  };
  folly::F14FastMap<std::string, ServiceEntry> services_;
  ServiceMatcher serviceMatcher_;
  std::string relayID_;
  std::shared_ptr<stats::StatsRegistry> statsRegistry_;
  std::shared_ptr<stats::MoQStatsCollector> statsCollector_;
};

} // namespace openmoq::moqx
