/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MoqxPicoRelayServer.h"

#include "stats/PicoQuicStatsCollector.h"
#include <folly/io/async/EventBase.h>
#include <folly/logging/xlog.h>
#include <moxygen/MoQRelaySession.h>
#include <moxygen/openmoq/transport/pico/PicoTransportConfig.h>

using namespace moxygen;

namespace openmoq::moqx {

namespace {

moxygen::PicoTransportConfig picoTransportConfigFromQuicConfig(const config::QuicConfig& quic) {
  return moxygen::PicoTransportConfig{
      .maxData = quic.maxData,
      .maxStreamData = quic.maxStreamData,
      .maxUniStreams = quic.maxUniStreams,
      .maxBidiStreams = quic.maxBidiStreams,
      .maxDatagramFrameSize = 1280, // not user-configurable; MoQ requires datagrams
      .idleTimeoutMs = quic.idleTimeoutMs,
      .maxAckDelayUs = quic.maxAckDelayUs,
      .minAckDelayUs = quic.minAckDelayUs,
      .defaultStreamPriority = quic.defaultStreamPriority,
      .defaultDatagramPriority = quic.defaultDatagramPriority,
      .ccAlgo = quic.ccAlgo,
  };
}

std::string resolveCert(const config::ListenerConfig& cfg) {
  return std::visit(
      [](const auto& tls) -> std::string {
        using T = std::decay_t<decltype(tls)>;
        if constexpr (std::is_same_v<T, config::Insecure>) {
          return "";
        } else {
          return tls.certFile;
        }
      },
      cfg.tlsMode
  );
}

std::string resolveKey(const config::ListenerConfig& cfg) {
  return std::visit(
      [](const auto& tls) -> std::string {
        using T = std::decay_t<decltype(tls)>;
        if constexpr (std::is_same_v<T, config::Insecure>) {
          return "";
        } else {
          return tls.keyFile;
        }
      },
      cfg.tlsMode
  );
}

} // namespace

MoqxPicoRelayServer::MoqxPicoRelayServer(
    const config::ListenerConfig& listenerCfg,
    std::shared_ptr<MoqxRelayContext> context,
    std::shared_ptr<folly::IOThreadPoolExecutor> ioExecutor
)
    : MoQPicoQuicEventBaseServer(
          resolveCert(listenerCfg),
          resolveKey(listenerCfg),
          listenerCfg.endpoint,
          ioExecutor->getAllEventBases()[0],
          listenerCfg.moqtVersions,
          picoTransportConfigFromQuicConfig(listenerCfg.quic),
          moxygen::PicoWebTransportConfig{
              .enableWebTransport = true,
              .enableQuicTransport = true,
              .wtEndpoints = context->getExactServicePaths(),
              // pico doesn't support session flow control, so limit to one per connection
              .wtMaxSessions = 1,
          }
      ),
      listenerCfg_(listenerCfg), context_(std::move(context)),
      evb_(ioExecutor->getAllEventBases()[0].get()) {}

MoqxPicoRelayServer::~MoqxPicoRelayServer() {
  context_->stop();
  evb_->runImmediatelyOrRunInEventBaseThreadAndWait([this] { MoQPicoQuicEventBaseServer::stop(); });
}

void MoqxPicoRelayServer::setStatsRegistry(std::shared_ptr<stats::StatsRegistry> registry) {
  context_->setStatsRegistry(registry);
  auto collector = stats::PicoQuicStatsCollector::create(std::move(registry), evb_);
  setPicoQuicStatsCallback(std::move(collector));
}

void MoqxPicoRelayServer::start() {
  evb_->runImmediatelyOrRunInEventBaseThreadAndWait([this] {
    MoQPicoQuicEventBaseServer::start(listenerCfg_.address);
  });
}

void MoqxPicoRelayServer::start(const folly::SocketAddress& /*addr*/) {
  start();
}

void MoqxPicoRelayServer::onNewSession(std::shared_ptr<MoQSession> clientSession) {
  context_->onNewSession(std::move(clientSession));
}

void MoqxPicoRelayServer::terminateClientSession(std::shared_ptr<MoQSession> session) {
  context_->onSessionEnd();
  MoQPicoQuicEventBaseServer::terminateClientSession(std::move(session));
}

folly::Expected<folly::Unit, SessionCloseErrorCode> MoqxPicoRelayServer::validateAuthority(
    const ClientSetup& clientSetup,
    uint64_t negotiatedVersion,
    std::shared_ptr<MoQSession> session
) {
  auto base =
      MoQPicoQuicEventBaseServer::validateAuthority(clientSetup, negotiatedVersion, session);
  if (!base.hasValue()) {
    return base;
  }
  return context_->validateAuthority(clientSetup, negotiatedVersion, std::move(session));
}

std::shared_ptr<MoQSession> MoqxPicoRelayServer::createSession(
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
