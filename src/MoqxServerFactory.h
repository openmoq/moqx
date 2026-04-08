#pragma once

#include <memory>

#include "MoqxPicoRelayServer.h"
#include "MoqxRelayContext.h"
#include "MoqxRelayServer.h"
#include "config/config.h"
#include "stats/StatsRegistry.h"
#include <folly/executors/IOThreadPoolExecutor.h>
#include <moxygen/MoQServerBase.h>

namespace openmoq::moqx {

// Creates the appropriate relay server for the given listener config and wires
// stats. Both stack paths accept the same ioExecutor for a uniform call site.
inline std::shared_ptr<moxygen::MoQServerBase> makeRelayServer(
    const config::ListenerConfig& listenerCfg,
    std::shared_ptr<MoqxRelayContext> context,
    std::shared_ptr<folly::IOThreadPoolExecutor> ioExecutor,
    std::shared_ptr<stats::StatsRegistry> statsRegistry
) {
  if (listenerCfg.quicStack == config::QuicStack::Picoquic) {
    // TODO: pass listenerCfg.quic settings to PicoRelayServer.
    // Requires moxygen to expose a PicoTransportParams struct (parallel to
    // PicoWebTransportConfig) so createQuicContext() can call
    // picoquic_set_default_tp_value() for each field.
    auto server = std::make_shared<MoqxPicoRelayServer>(
        listenerCfg,
        std::move(context),
        std::move(ioExecutor)
    );
    server->setStatsRegistry(std::move(statsRegistry));
    return server;
  }
  auto server =
      std::make_shared<MoqxRelayServer>(listenerCfg, std::move(context), std::move(ioExecutor));
  server->setStatsRegistry(std::move(statsRegistry));
  return server;
}

} // namespace openmoq::moqx
