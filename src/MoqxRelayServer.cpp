#include <moqx/MoqxRelayServer.h>
#include <moqx/stats/MoQStatsCollector.h>
#include <moxygen/MoQRelaySession.h>
#include <moxygen/events/MoQFollyExecutorImpl.h>
#include <moxygen/util/InsecureVerifierDangerousDoNotUseInProduction.h>

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
}

namespace {

// Relay chaining requires draft 16+ for wildcard subscribeNamespace and
// NAMESPACE messages on the bidi stream. Connections negotiating an earlier
// draft will not receive namespace announcements from the upstream relay.
std::shared_ptr<fizz::CertificateVerifier> makeUpstreamVerifier(
    const config::UpstreamTlsConfig& tls) {
  if (tls.insecure) {
    return std::make_shared<
        moxygen::test::InsecureVerifierDangerousDoNotUseInProduction>();
  }
  if (tls.caCertFile) {
    // TODO: load custom CA cert via fizz OpenSSLCertUtils / X509 store
    XLOG(WARN) << "upstream.tls.ca_cert is not yet implemented; "
                  "using system CAs";
  }
  return nullptr; // nullptr = fizz uses system CAs
}

} // namespace

void MoqxRelayServer::initUpstream(
    const config::UpstreamConfig& cfg,
    const std::string& relayID) {
  auto evbs = getWorkerEvbs();
  CHECK(!evbs.empty()) << "initUpstream must be called after start()";

  auto verifier = makeUpstreamVerifier(cfg.tls);

  // Use the first worker EVB for all upstream connections.
  // Per-EVB providers (one per worker thread) are a follow-up.
  auto exec = std::make_shared<MoQFollyExecutorImpl>(evbs[0]);

  for (auto& [name, relay] : relays_) {
    auto provider = std::make_shared<UpstreamProvider>(
        exec,
        proxygen::URL(cfg.url),
        /*publishHandler=*/relay,
        /*subscribeHandler=*/relay,
        verifier,
        relayID);
    relay->setUpstreamProvider(provider);

    // Eagerly connect so the peering handshake fires before any subscribers
    // arrive. The connection is lazy in UpstreamProvider but we kick it off
    // now so the upstream namespace tree is ready.
    co_withExecutor(evbs[0], provider->start()).start();
  }
}

void MoqxRelayServer::onNewSession(std::shared_ptr<MoQSession> clientSession) {
  // Relay handler routing deferred to validateAuthority() where authority is available

  if (statsRegistry_) {
    if (!statsCollector_) {
      statsCollector_ = stats::MoQStatsCollector::create_moq_stats_collector(
          folly::getKeepAliveToken(*clientSession->getExecutor()),
          statsRegistry_
      );
      // For now we only want to allow one collector registration as we have only one executor
      statsRegistry_->lock();
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
