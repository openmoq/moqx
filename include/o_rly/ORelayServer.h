#pragma once

#include <moxygen/MoQServer.h>
#include <o_rly/ORelay.h>
#include <o_rly/ServiceMatcher.h>
#include <o_rly/config/config.h>

namespace openmoq::o_rly {

class ORelayServer : public moxygen::MoQServer {
public:
  // Used when the insecure flag is false
  ORelayServer(
      const std::string& cert,
      const std::string& key,
      const std::string& endpoint,
      const std::string& versions,
      std::vector<config::ServiceConfig> services
  );

  // Used when the insecure flag is true
  ORelayServer(
      const std::string& endpoint,
      const std::string& versions,
      std::vector<config::ServiceConfig> services
  );

  void onNewSession(std::shared_ptr<moxygen::MoQSession> clientSession) override;

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
  void initRelays(const std::vector<config::ServiceConfig>& services);

  std::vector<std::shared_ptr<ORelay>> relays_;
  ServiceMatcher serviceMatcher_;
};

} // namespace openmoq::o_rly
