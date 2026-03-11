#include <moxygen/MoQRelaySession.h>
#include <o_rly/ORelayServer.h>
#include <o_rly/stats/MoQStatsCollector.h>

using namespace moxygen;

namespace openmoq::o_rly {

ORelayServer::ORelayServer(
    const std::string& cert,
    const std::string& key,
    const std::string& endpoint,
    const std::string& versions,
    size_t maxCachedTracks,
    size_t maxCachedGroupsPerTrack
)
    : MoQServer(
          quic::samples::createFizzServerContext(
              [&versions]() {
                std::vector<std::string> alpns = {"h3"};
                auto moqt = getMoqtProtocols(versions, true);
                alpns.insert(alpns.end(), moqt.begin(), moqt.end());
                return alpns;
              }(),
              fizz::server::ClientAuthMode::Optional,
              cert,
              key
          ),
          endpoint
      ),
      relay_(std::make_shared<ORelay>(maxCachedTracks, maxCachedGroupsPerTrack)) {}

ORelayServer::ORelayServer(
    const std::string& endpoint,
    const std::string& versions,
    size_t maxCachedTracks,
    size_t maxCachedGroupsPerTrack
)
    : MoQServer(
          quic::samples::createFizzServerContextWithInsecureDefault(
              [&versions]() {
                std::vector<std::string> alpns = {"h3"};
                auto moqt = getMoqtProtocols(versions, true);
                alpns.insert(alpns.end(), moqt.begin(), moqt.end());
                return alpns;
              }(),
              fizz::server::ClientAuthMode::None,
              "" /* cert */,
              "" /* key */
          ),
          endpoint
      ),
      relay_(std::make_shared<ORelay>(maxCachedTracks, maxCachedGroupsPerTrack)) {}

void ORelayServer::setStatsRegistry(std::shared_ptr<stats::StatsRegistry> registry) {
  statsRegistry_ = std::move(registry);
}

void ORelayServer::onNewSession(std::shared_ptr<MoQSession> clientSession) {
  clientSession->setPublishHandler(relay_);
  clientSession->setSubscribeHandler(relay_);

  if (statsRegistry_) {
    // Create a collector for this session and register it with the registry.
    // Both publisher and subscriber callbacks share the same collector so
    // each session is counted once.
    auto collector = std::make_shared<stats::MoQStatsCollector>(
        folly::getKeepAliveToken(*clientSession->getExecutor()),
        statsRegistry_
    );
    statsRegistry_->registerCollector(collector);

    clientSession->setPublisherStatsCallback(collector);
    clientSession->setSubscriberStatsCallback(collector);

    statsRegistry_->onNewSession();
  }
}

void ORelayServer::terminateClientSession(std::shared_ptr<MoQSession> session) {
  if (statsRegistry_) {
    statsRegistry_->onTerminateSession();
  }
  MoQServer::terminateClientSession(std::move(session));
}

std::shared_ptr<MoQSession> ORelayServer::createSession(
    folly::MaybeManagedPtr<proxygen::WebTransport> wt,
    std::shared_ptr<MoQExecutor> executor
) {
  return std::make_shared<MoQRelaySession>(
      folly::MaybeManagedPtr<proxygen::WebTransport>(std::move(wt)),
      *this,
      std::move(executor)
  );
}

} // namespace openmoq::o_rly
