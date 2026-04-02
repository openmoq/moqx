#include <moqx/MoqxRelayServer.h>
#include <moqx/stats/MoQStatsCollector.h>
#include <moqx/stats/QuicStatsCollector.h>
#include <moxygen/MoQRelaySession.h>

#include <folly/logging/xlog.h>

using namespace moxygen;

namespace openmoq::moqx {

namespace {

std::vector<std::string> buildAlpns(const std::string& versions) {
  std::vector<std::string> alpns = {"h3"};
  auto moqt = getMoqtProtocols(versions, true);
  alpns.insert(alpns.end(), moqt.begin(), moqt.end());
  return alpns;
}

} // namespace

void MoqxRelayServer::initRelays(
    const folly::F14FastMap<std::string, config::ServiceConfig>& services
) {
  for (const auto& [name, svc] : services) {
    relays_.emplace(
        name,
        std::make_shared<MoqxRelay>(svc.cache.maxCachedTracks, svc.cache.maxCachedGroupsPerTrack)
    );
  }
}

MoqxRelayServer::MoqxRelayServer(
    const std::string& cert,
    const std::string& key,
    const std::string& endpoint,
    const std::string& versions,
    folly::F14FastMap<std::string, config::ServiceConfig> services
)
    : MoQServer(
          quic::samples::createFizzServerContext(
              buildAlpns(versions),
              fizz::server::ClientAuthMode::Optional,
              cert,
              key
          ),
          endpoint
      ),
      serviceMatcher_(services) {
  initRelays(services);
}

MoqxRelayServer::MoqxRelayServer(
    const std::string& endpoint,
    const std::string& versions,
    folly::F14FastMap<std::string, config::ServiceConfig> services
)
    : MoQServer(
          quic::samples::createFizzServerContextWithInsecureDefault(
              buildAlpns(versions),
              fizz::server::ClientAuthMode::None,
              "" /* cert */,
              "" /* key */
          ),
          endpoint
      ),
      serviceMatcher_(services) {
  initRelays(services);
}

void MoqxRelayServer::setStatsRegistry(std::shared_ptr<stats::StatsRegistry> registry) {
  statsRegistry_ = std::move(registry);

  if (statsRegistry_) {
    // Register the MoQ collector
    statsCollector_ = stats::MoQStatsCollector::create_moq_stats_collector(statsRegistry_);

    // Wire up QUIC transport stats factory
    setQuicStatsFactory(std::make_unique<stats::QuicStatsCollector::Factory>(statsRegistry_));
  }
}

void MoqxRelayServer::onNewSession(std::shared_ptr<MoQSession> clientSession) {
  // Relay handler routing deferred to validateAuthority() where authority is available

  if (statsRegistry_) {
    if (!statsCollector_->owningExecutor()) {
      // First session: bind the executor now that we have one.
      statsCollector_->setExecutor(clientSession->getExecutor());
    }

    clientSession->setPublisherStatsCallback(statsCollector_->publisherCallback());
    clientSession->setSubscriberStatsCallback(statsCollector_->subscriberCallback());

    statsCollector_->onSessionStart();
  }
}

void MoqxRelayServer::terminateClientSession(std::shared_ptr<MoQSession> session) {
  if (statsCollector_) {
    statsCollector_->onSessionEnd();
  }
  MoQServer::terminateClientSession(std::move(session));
}

folly::Expected<folly::Unit, SessionCloseErrorCode> MoqxRelayServer::validateAuthority(
    const ClientSetup& clientSetup,
    uint64_t negotiatedVersion,
    std::shared_ptr<MoQSession> session
) {
  // Base class format validation
  auto base = MoQServer::validateAuthority(clientSetup, negotiatedVersion, session);
  if (!base.hasValue()) {
    return base;
  }

  // Match service by authority + path
  const auto& authority = session->getAuthority();
  const auto& path = session->getPath();
  auto matchedName = serviceMatcher_.match(authority, path);
  if (!matchedName) {
    XLOG(ERR) << "No service matched authority=" << authority << " path=" << path;
    return folly::makeUnexpected(SessionCloseErrorCode::INVALID_AUTHORITY);
  }

  // Route: set per-service relay as handler
  auto it = relays_.find(*matchedName);
  CHECK(it != relays_.end()) << "Service '" << *matchedName << "' matched but no relay found";
  session->setPublishHandler(it->second);
  session->setSubscribeHandler(it->second);
  return folly::unit;
}

std::shared_ptr<MoQSession> MoqxRelayServer::createSession(
    folly::MaybeManagedPtr<proxygen::WebTransport> wt,
    std::shared_ptr<MoQExecutor> executor
) {
  return std::make_shared<MoQRelaySession>(
      folly::MaybeManagedPtr<proxygen::WebTransport>(std::move(wt)),
      *this,
      std::move(executor)
  );
}

} // namespace openmoq::moqx
