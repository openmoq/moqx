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

void MoqxRelayServer::initServices(
    const folly::F14FastMap<std::string, config::ServiceConfig>& services,
    const std::string& relayID
) {
  for (const auto& [name, svc] : services) {
    services_.emplace(
        name,
        ServiceEntry{
            svc,
            std::make_shared<MoqxRelay>(
                svc.cache.maxCachedTracks,
                svc.cache.maxCachedGroupsPerTrack,
                relayID
            )
        }
    );
  }
}

MoqxRelayServer::MoqxRelayServer(
    const std::string& cert,
    const std::string& key,
    const std::string& endpoint,
    const std::string& versions,
    folly::F14FastMap<std::string, config::ServiceConfig> services,
    const std::string& relayID
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
      serviceMatcher_(services), relayID_(relayID) {
  initServices(services, relayID);
}

MoqxRelayServer::MoqxRelayServer(
    const std::string& endpoint,
    const std::string& versions,
    folly::F14FastMap<std::string, config::ServiceConfig> services,
    const std::string& relayID
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
      serviceMatcher_(services), relayID_(relayID) {
  initServices(services, relayID);
}

MoqxRelayServer::~MoqxRelayServer() {
  // 1. Signal upstreams to stop — cancels reconnect backoff sleep so the
  //    reconnect coroutine on the worker EVB can exit cleanly.
  for (auto& [name, entry] : services_) {
    entry.relay->stop();
  }
  // 2. Close incoming connections, drain worker EVBs (terminateClientSession
  //    and ~MoQSession complete here), then destroy EVBs.
  MoQServer::stop();
}

void MoqxRelayServer::setStatsRegistry(std::shared_ptr<stats::StatsRegistry> registry) {
  statsRegistry_ = std::move(registry);
}

namespace {

// Relay chaining requires draft 16+ for wildcard subscribeNamespace and
// NAMESPACE messages on the bidi stream. Connections negotiating an earlier
// draft will not receive namespace announcements from the upstream relay.
std::shared_ptr<fizz::CertificateVerifier> makeUpstreamVerifier(const config::UpstreamTlsConfig& tls
) {
  if (tls.insecure) {
    return std::make_shared<moxygen::test::InsecureVerifierDangerousDoNotUseInProduction>();
  }
  if (tls.caCertFile) {
    // TODO: load custom CA cert via fizz OpenSSLCertUtils / X509 store
    XLOG(WARN) << "upstream.tls.ca_cert is not yet implemented; "
                  "using system CAs";
  }
  return nullptr; // nullptr = fizz uses system CAs
}

} // namespace

void MoqxRelayServer::initUpstreams() {
  auto evbs = getWorkerEvbs();
  CHECK(!evbs.empty()) << "initUpstreams must be called after start()";

  // Use the first worker EVB for all upstream connections.
  // Per-EVB providers (one per worker thread) are a follow-up.
  auto exec = std::make_shared<MoQFollyExecutorImpl>(evbs[0]);

  for (auto& [name, entry] : services_) {
    if (!entry.config.upstream) {
      continue;
    }
    const auto& cfg = *entry.config.upstream;
    auto verifier = makeUpstreamVerifier(cfg.tls);
    auto relay = entry.relay;
    auto onConnect = [relay](std::shared_ptr<MoQSession> session) -> folly::coro::Task<void> {
      co_await relay->onUpstreamConnect(session);
    };
    auto onDisconnect = [relay]() { relay->onUpstreamDisconnect(); };
    auto provider = std::make_shared<UpstreamProvider>(
        exec,
        proxygen::URL(cfg.url),
        /*publishHandler=*/entry.relay,
        /*subscribeHandler=*/entry.relay,
        verifier,
        std::move(onConnect),
        std::move(onDisconnect),
        cfg.connectTimeout,
        cfg.idleTimeout
    );
    entry.relay->setUpstreamProvider(provider);

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
  auto it = services_.find(*matchedName);
  CHECK(it != services_.end()) << "Service '" << *matchedName << "' matched but no entry found";
  session->setPublishHandler(it->second.relay);
  session->setSubscribeHandler(it->second.relay);
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
