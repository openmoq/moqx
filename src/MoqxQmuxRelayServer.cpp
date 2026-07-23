/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MoqxQmuxRelayServer.h"

#include "stats/EventBaseStatsCollector.h"
#include <moxygen/MoQRelaySession.h>
#include <moxygen/QmuxUtils.h>
#include <proxygen/httpserver/samples/hq/FizzContext.h>

#include <folly/logging/xlog.h>
#include <quic/state/TransportSettings.h>

using namespace moxygen;

namespace openmoq::moqx {

namespace {

// qmux advertises only the MoQ ALPNs (no "h3" — that is for HTTP/3 WebTransport
// on the QUIC path).
std::vector<std::string> buildQmuxAlpns(const std::string& versions) {
  return getMoqtProtocols(versions, /*useStandard=*/true);
}

std::shared_ptr<const fizz::server::FizzServerContext>
buildFizzContext(const config::ListenerConfig& cfg) {
  auto alpns = buildQmuxAlpns(cfg.moqtVersions);
  return std::visit(
      [&alpns](const auto& tls) -> std::shared_ptr<const fizz::server::FizzServerContext> {
        using T = std::decay_t<decltype(tls)>;
        if constexpr (std::is_same_v<T, config::Insecure>) {
          return quic::samples::createFizzServerContextWithInsecureDefault(
              alpns,
              fizz::server::ClientAuthMode::None,
              "",
              ""
          );
        } else {
          return quic::samples::createFizzServerContext(
              alpns,
              fizz::server::ClientAuthMode::Optional,
              tls.certFile,
              tls.keyFile
          );
        }
      },
      cfg.tlsMode
  );
}

// Translate the QUIC flow-control / idle-timeout knobs into the QMUX transport
// params advertised to the peer. mvfst-only tunables don't apply over TCP.
MoQQmuxServer::Config buildQmuxConfig(const config::QuicConfig& quic) {
  quic::TransportSettings ts;
  ts.advertisedInitialConnectionFlowControlWindow = quic.maxData;
  ts.advertisedInitialBidiLocalStreamFlowControlWindow = quic.maxStreamData;
  ts.advertisedInitialBidiRemoteStreamFlowControlWindow = quic.maxStreamData;
  ts.advertisedInitialUniStreamFlowControlWindow = quic.maxStreamData;
  ts.advertisedInitialMaxStreamsBidi = quic.maxBidiStreams;
  ts.advertisedInitialMaxStreamsUni = quic.maxUniStreams;
  ts.idleTimeout = std::chrono::milliseconds(quic.idleTimeoutMs);

  MoQQmuxServer::Config config;
  config.selfTransportParams = qmuxParamsFromTransportSettings(ts);
  return config;
}

} // namespace

MoqxQmuxRelayServer::MoqxQmuxRelayServer(
    const config::ListenerConfig& listenerCfg,
    std::shared_ptr<MoqxRelayContext> context,
    folly::IOThreadPoolExecutor* ioExecutor
)
    : MoQQmuxServer(
          listenerCfg.endpoint,
          buildFizzContext(listenerCfg),
          buildQmuxConfig(listenerCfg.quic)
      ),
      listenerCfg_(listenerCfg), context_(std::move(context)), ioExecutor_(ioExecutor) {
  addSetupParameter(SetupParameter(folly::to_underlying(SetupKey::RELAY_HOPS), std::string{}));
}

MoqxQmuxRelayServer::~MoqxQmuxRelayServer() {
  // Close incoming connections, drain worker EVBs, then destroy EVBs.
  stop();
}

void MoqxQmuxRelayServer::stop() {
  if (stopped_) {
    return;
  }
  stopped_ = true;
  // Keep context_ alive: terminateClientSession can run after stop() returns,
  // from handleClientSession coroutines still draining on IO threads.
  MoQQmuxServer::stop();
}

void MoqxQmuxRelayServer::setStatsRegistry(std::shared_ptr<stats::StatsRegistry> registry) {
  context_->setStatsRegistry(registry);
  for (auto& ka : ioExecutor_->getAllEventBases()) {
    stats::EventBaseStatsCollector::create(registry, ka.get());
  }
  // No QuicStatsCollector: qmux runs over TCP, not the mvfst QUIC transport.
}

void MoqxQmuxRelayServer::start() {
  auto evbKAs = ioExecutor_->getAllEventBases();
  std::vector<folly::EventBase*> evbs;
  evbs.reserve(evbKAs.size());
  for (auto& ka : evbKAs) {
    evbs.push_back(ka.get());
  }
  ioExecutor_ = nullptr;
  MoQQmuxServer::start(listenerCfg_.address, std::move(evbs));
}

void MoqxQmuxRelayServer::start(const folly::SocketAddress& /*addr*/) {
  start();
}

void MoqxQmuxRelayServer::onNewSession(std::shared_ptr<MoQSession> clientSession) {
  context_->onNewSession(std::move(clientSession));
}

void MoqxQmuxRelayServer::terminateClientSession(std::shared_ptr<MoQSession> session) {
  context_->onSessionEnd(session);
  MoQQmuxServer::terminateClientSession(std::move(session));
}

folly::Expected<folly::Unit, SessionCloseErrorCode> MoqxQmuxRelayServer::validateAuthority(
    const ClientSetup& clientSetup,
    uint64_t negotiatedVersion,
    std::shared_ptr<MoQSession> session
) {
  // Base class format validation (lives on MoQServerBase).
  auto base = MoQQmuxServer::validateAuthority(clientSetup, negotiatedVersion, session);
  if (!base.hasValue()) {
    return base;
  }
  return context_->validateAuthority(clientSetup, negotiatedVersion, std::move(session));
}

std::shared_ptr<MoQSession> MoqxQmuxRelayServer::createSession(
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
