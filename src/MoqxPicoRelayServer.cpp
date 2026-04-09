#include "MoqxPicoRelayServer.h"

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
      listenerCfg_(listenerCfg), context_(std::move(context)) {}

MoqxPicoRelayServer::~MoqxPicoRelayServer() {
  context_->stop();
  MoQPicoQuicEventBaseServer::stop();
}

void MoqxPicoRelayServer::start() {
  MoQPicoQuicEventBaseServer::start(listenerCfg_.address);
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
