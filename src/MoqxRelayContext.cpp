#include <moqx/MoqxRelayContext.h>
#include <moqx/stats/MoQStatsCollector.h>
#include <moxygen/events/MoQFollyExecutorImpl.h>
#include <moxygen/util/InsecureVerifierDangerousDoNotUseInProduction.h>

#include <folly/logging/xlog.h>

using namespace moxygen;

namespace openmoq::moqx {

MoqxRelayContext::MoqxRelayContext(
    const folly::F14FastMap<std::string, config::ServiceConfig>& services,
    const std::string& relayID
)
    : serviceMatcher_(services), relayID_(relayID) {
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

void MoqxRelayContext::setStatsRegistry(std::shared_ptr<stats::StatsRegistry> registry) {
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
    XLOG(WARN) << "upstream.tls.ca_cert is not yet implemented; using system CAs";
  }
  return nullptr; // nullptr = fizz uses system CAs
}

} // namespace

void MoqxRelayContext::initUpstreams(folly::EventBase* workerEvb) {
  CHECK(workerEvb) << "initUpstreams: workerEvb must not be null";

  // Use the provided worker EVB for all upstream connections.
  // Per-EVB providers (one per worker thread) are a follow-up.
  auto exec = std::make_shared<MoQFollyExecutorImpl>(workerEvb);

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
    co_withExecutor(workerEvb, provider->start()).start();
  }
}

void MoqxRelayContext::stop() {
  for (auto& [name, entry] : services_) {
    entry.relay->stop();
  }
}

size_t MoqxRelayContext::clearCaches(std::string_view serviceName) {
  CHECK(workerEvb_) << "clearCaches called before setWorkerEvb";
  auto* evb = workerEvb_;
  if (!serviceName.empty()) {
    auto it = services_.find(std::string(serviceName));
    if (it == services_.end()) {
      return 0;
    }
    evb->runInEventBaseThreadAndWait([&] { it->second.relay->clearCache(); });
    return 1;
  }
  for (auto& [name, entry] : services_) {
    evb->runInEventBaseThreadAndWait([&] { entry.relay->clearCache(); });
  }
  return services_.size();
}

void MoqxRelayContext::onNewSession(std::shared_ptr<MoQSession> clientSession) {
  if (statsRegistry_) {
    if (!statsCollector_) {
      statsCollector_ = stats::MoQStatsCollector::create_moq_stats_collector(statsRegistry_);
    }
    if (!statsCollector_->owningExecutor()) {
      statsCollector_->setExecutor(clientSession->getExecutor());
    }

    clientSession->setPublisherStatsCallback(statsCollector_->publisherCallback());
    clientSession->setSubscriberStatsCallback(statsCollector_->subscriberCallback());

    statsCollector_->onSessionStart();
  }
}

void MoqxRelayContext::onSessionEnd() {
  if (statsCollector_) {
    statsCollector_->onSessionEnd();
  }
}

folly::Expected<folly::Unit, SessionCloseErrorCode> MoqxRelayContext::validateAuthority(
    const ClientSetup& /*clientSetup*/,
    uint64_t /*negotiatedVersion*/,
    std::shared_ptr<MoQSession> session
) {
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

} // namespace openmoq::moqx
