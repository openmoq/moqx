#pragma once

#include <memory>
#include <optional>

#include <folly/executors/IOThreadPoolExecutor.h>
#include <moqx/MoqxPicoRelayServer.h>
#include <moqx/MoqxRelayContext.h>
#include <moqx/MoqxRelayServer.h>
#include <moqx/config/config.h>
#include <moxygen/MoQServerBase.h>
#include <quic/server/QuicServerTransport.h>

namespace openmoq::moqx {

// Creates the appropriate relay server for the given listener config.
// Both stack paths accept the same ioExecutor for a uniform call site.
inline std::shared_ptr<moxygen::MoQServerBase> makeRelayServer(
    const config::ListenerConfig& listenerCfg,
    std::shared_ptr<MoqxRelayContext> context,
    std::shared_ptr<folly::IOThreadPoolExecutor> ioExecutor,
    std::optional<quic::TransportSettings> transportSettings = std::nullopt
) {
  if (listenerCfg.quicStack == config::QuicStack::Picoquic) {
    // TODO: pass transportSettings to PicoRelayServer when supported
    return std::make_shared<MoqxPicoRelayServer>(
        listenerCfg,
        std::move(context),
        std::move(ioExecutor)
    );
  }
  return std::make_shared<MoqxRelayServer>(
      listenerCfg,
      std::move(context),
      std::move(ioExecutor),
      std::move(transportSettings)
  );
}

} // namespace openmoq::moqx
