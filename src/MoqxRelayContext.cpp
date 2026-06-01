/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MoqxRelayContext.h"
#include "relay/PublisherCrossExecFilter.h"
#include "relay/RelayExecUtil.h"
#include "relay/SubscriberCrossExecFilter.h"
#include "stats/MoQStatsCollector.h"
#include <moxygen/events/MoQFollyExecutorImpl.h>
#include <moxygen/util/InsecureVerifierDangerousDoNotUseInProduction.h>

#include <folly/coro/Task.h>
#include <folly/executors/thread_factory/NamedThreadFactory.h>
#include <folly/logging/xlog.h>

using namespace moxygen;

namespace openmoq::moqx {

MoqxRelayContext::MoqxRelayContext(
    const folly::F14FastMap<std::string, config::ServiceConfig>& services,
    const std::string& relayID,
    bool useRelayThread
)
    : serviceMatcher_(services), relayID_(relayID) {
  if (useRelayThread && !services.empty()) {
    relayThreadPool_ = std::make_shared<folly::IOThreadPoolExecutor>(
        services.size(),
        std::make_shared<folly::NamedThreadFactory>("moqx-relay")
    );
    auto evbs = relayThreadPool_->getAllEventBases();
    XCHECK_EQ(evbs.size(), services.size());
    size_t i = 0;
    for (const auto& [name, svc] : services) {
      auto relay = std::make_shared<MoqxRelay>(svc.cache, relayID);
      relay->setRelayExec(std::make_shared<moxygen::MoQFollyExecutorImpl>(evbs[i++].get()));
      services_.emplace(name, ServiceEntry{svc, std::move(relay)});
    }
  } else {
    for (const auto& [name, svc] : services) {
      services_.emplace(name, ServiceEntry{svc, std::make_shared<MoqxRelay>(svc.cache, relayID)});
    }
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
  workerEvb_ = workerEvb;

  auto workerExec = std::make_shared<moxygen::MoQFollyExecutorImpl>(workerEvb);
  for (auto& [name, entry] : services_) {
    if (!entry.config.upstream) {
      continue;
    }
    const auto& cfg = *entry.config.upstream;
    auto verifier = makeUpstreamVerifier(cfg.tls);
    auto relay = entry.relay;
    auto* relayExec = relay->getRelayExec();
    auto onConnect = [relay,
                      relayExec](std::shared_ptr<MoQSession> session) -> folly::coro::Task<void> {
      if (relayExec) {
        co_return co_await folly::coro::co_withExecutor(
            folly::getKeepAliveToken(relayExec),
            relay->onUpstreamConnect(session)
        );
      }
      co_return co_await relay->onUpstreamConnect(session);
    };
    auto onDisconnect = [relay, relayExec]() {
      runOnExec(relayExec, [relay]() { relay->onUpstreamDisconnect(); });
    };
    std::shared_ptr<moxygen::Publisher> pubHandler = relay;
    std::shared_ptr<moxygen::Subscriber> subHandler = relay;
    if (relayExec) {
      pubHandler = std::make_shared<PublisherCrossExecFilter>(relayExec, relay);
      subHandler = std::make_shared<SubscriberCrossExecFilter>(relayExec, relay);
    }
    auto provider = std::make_shared<UpstreamProvider>(
        workerExec,
        proxygen::URL(cfg.url),
        /*publishHandler=*/pubHandler,
        /*subscribeHandler=*/subHandler,
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
    co_withExecutor(workerExec.get(), provider->start()).start();
  }
}

void MoqxRelayContext::stop() {
  for (auto& [name, entry] : services_) {
    entry.relay->stop();
  }
}

folly::coro::Task<size_t> MoqxRelayContext::purgeCache(
    std::string_view serviceName,
    std::optional<moxygen::FullTrackName> ftn,
    std::optional<moxygen::TrackNamespace> ns
) {
  auto purgeServiceCache = [&](MoqxRelay& r) -> size_t {
    if (ftn) {
      return r.purge(*ftn);
    }
    if (ns) {
      return r.purge(*ns);
    }
    return r.purge();
  };
  size_t total = 0;
  if (!serviceName.empty()) {
    if (auto it = services_.find(std::string(serviceName)); it != services_.end()) {
      total = purgeServiceCache(*it->second.relay);
    }
  } else {
    for (auto& [name, entry] : services_) {
      XLOG(DBG1) << "Purging service: " << name;
      total += purgeServiceCache(*entry.relay);
    }
  }
  co_return total;
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

  // Route: set per-service relay as handler, wrapping in cross-exec filters if needed
  auto it = services_.find(*matchedName);
  CHECK(it != services_.end()) << "Service '" << *matchedName << "' matched but no entry found";
  auto* relayExec = it->second.relay->getRelayExec();
  if (relayExec) {
    session->setPublishHandler(
        std::make_shared<PublisherCrossExecFilter>(relayExec, it->second.relay)
    );
    session->setSubscribeHandler(
        std::make_shared<SubscriberCrossExecFilter>(relayExec, it->second.relay)
    );
  } else {
    session->setPublishHandler(it->second.relay);
    session->setSubscribeHandler(it->second.relay);
  }
  return folly::unit;
}

std::vector<std::string> MoqxRelayContext::getExactServicePaths() const {
  return serviceMatcher_.allExactPaths();
}

void MoqxRelayContext::dumpState(RelayContextVisitor& visitor) const {
  int64_t activeSessions = 0;
  if (statsCollector_) {
    activeSessions = statsCollector_->snapshot().moqActiveSessions;
  }

  visitor.onRelayBegin(relayID_, activeSessions);
  for (const auto& [name, entry] : services_) {
    RelayStateVisitor& rv = visitor.onServiceBegin(name);
    entry.relay->dumpState(rv);
    if (entry.config.upstream) {
      auto up = entry.relay->upstreamProvider();
      visitor.onServiceUpstream(
          entry.config.upstream->url,
          up ? up->stateString() : "disconnected"
      );
    }
    visitor.onServiceEnd();
  }
  visitor.onRelayEnd();
}

} // namespace openmoq::moqx
