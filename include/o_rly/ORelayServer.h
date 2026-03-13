#pragma once

#include <memory>

#include <moxygen/MoQServer.h>
#include <o_rly/ORelay.h>
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
      size_t maxCachedTracks,
      size_t maxCachedGroupsPerTrack
  );

  // Used when the insecure flag is true
  ORelayServer(
      const std::string& endpoint,
      const std::string& versions,
      size_t maxCachedTracks,
      size_t maxCachedGroupsPerTrack
  );

  void setStatsRegistry(std::shared_ptr<stats::StatsRegistry> registry);

  void onNewSession(std::shared_ptr<moxygen::MoQSession> clientSession) override;

  void terminateClientSession(std::shared_ptr<moxygen::MoQSession> session) override;

protected:
  std::shared_ptr<moxygen::MoQSession> createSession(
      folly::MaybeManagedPtr<proxygen::WebTransport> wt,
      std::shared_ptr<moxygen::MoQExecutor> executor
  ) override;

private:
  std::shared_ptr<ORelay> relay_;
  std::shared_ptr<stats::StatsRegistry> statsRegistry_;
  std::shared_ptr<stats::MoQStatsCollector> statsCollector_;
};

} // namespace openmoq::o_rly
