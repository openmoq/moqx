#pragma once

#include <memory>

#include <moxygen/MoQServer.h>
#include <moqx/MoqxRelay.h>
#include <moqx/ServiceMatcher.h>
#include <moqx/config/config.h>
#include <moqx/stats/MoQStatsCollector.h>
#include <moqx/stats/StatsRegistry.h>

namespace openmoq::moqx {

class MoqxRelayServer : public moxygen::MoQServer {
public:
  // Used when the insecure flag is false
  MoqxRelayServer(
      const std::string& cert,
      const std::string& key,
      const std::string& endpoint,
      const std::string& versions,
      folly::F14FastMap<std::string, config::ServiceConfig> services
  );

  // Used when the insecure flag is true
  MoqxRelayServer(
      const std::string& endpoint,
      const std::string& versions,
      folly::F14FastMap<std::string, config::ServiceConfig> services
  );

  void setStatsRegistry(std::shared_ptr<stats::StatsRegistry> registry);

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
  void initRelays(const folly::F14FastMap<std::string, config::ServiceConfig>& services);

  folly::F14FastMap<std::string, std::shared_ptr<MoqxRelay>> relays_;
  ServiceMatcher serviceMatcher_;
  std::shared_ptr<stats::StatsRegistry> statsRegistry_;
  std::shared_ptr<stats::MoQStatsCollector> statsCollector_;
};

} // namespace openmoq::moqx
