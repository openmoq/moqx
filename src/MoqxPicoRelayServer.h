#pragma once

#include <memory>

#include "MoqxRelayContext.h"
#include "config/config.h"
#include <folly/executors/IOThreadPoolExecutor.h>
#include <moxygen/events/MoQExecutor.h>
#include <moxygen/openmoq/transport/pico/MoQPicoQuicEventBaseServer.h>
#include <proxygen/lib/http/webtransport/WebTransport.h>

namespace openmoq::moqx {

// MoQ relay server backed by the picoquic QUIC stack.
//
// Picoquic lacks packet steering across threads, so even though the full
// IOThreadPoolExecutor is passed in (for API symmetry with MoqxRelayServer),
// the server always pins itself to evbKAs[0] internally.
class MoqxPicoRelayServer : public moxygen::MoQPicoQuicEventBaseServer {
public:
  MoqxPicoRelayServer(
      const config::ListenerConfig& listenerCfg,
      std::shared_ptr<MoqxRelayContext> context,
      std::shared_ptr<folly::IOThreadPoolExecutor> ioExecutor
  );

  ~MoqxPicoRelayServer() override;

  // Preferred entry point: binds the address from the stored ListenerConfig.
  void start();

  // Satisfies MoQServerBase pure virtual; delegates to start().
  void start(const folly::SocketAddress& addr) override;

  void onNewSession(std::shared_ptr<moxygen::MoQSession> clientSession) override;

  void terminateClientSession(std::shared_ptr<moxygen::MoQSession> session) override;

  folly::Expected<folly::Unit, moxygen::SessionCloseErrorCode> validateAuthority(
      const moxygen::ClientSetup& clientSetup,
      uint64_t negotiatedVersion,
      std::shared_ptr<moxygen::MoQSession> session
  ) override;

protected:
  std::shared_ptr<moxygen::MoQSession> createSession(
      folly::MaybeManagedPtr<proxygen::WebTransport> wt,
      std::shared_ptr<moxygen::MoQExecutor> executor
  ) override;

private:
  config::ListenerConfig listenerCfg_;
  std::shared_ptr<MoqxRelayContext> context_;
};

} // namespace openmoq::moqx
