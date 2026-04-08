#pragma once

#include <memory>

#include "MoqxPicoRelayServer.h"
#include "MoqxRelayContext.h"
#include "MoqxRelayServer.h"
#include "config/config.h"
#include <folly/executors/IOThreadPoolExecutor.h>
#include <moxygen/MoQServerBase.h>
namespace openmoq::moqx {

// Creates the appropriate relay server for the given listener config.
// Both stack paths accept the same ioExecutor for a uniform call site.
inline std::shared_ptr<moxygen::MoQServerBase> makeRelayServer(
    const config::ListenerConfig& listenerCfg,
    std::shared_ptr<MoqxRelayContext> context,
    std::shared_ptr<folly::IOThreadPoolExecutor> ioExecutor
) {
  if (listenerCfg.quicStack == config::QuicStack::Picoquic) {
    return std::make_shared<MoqxPicoRelayServer>(
        listenerCfg,
        std::move(context),
        std::move(ioExecutor)
    );
  }
  return std::make_shared<MoqxRelayServer>(listenerCfg, std::move(context), std::move(ioExecutor));
}

} // namespace openmoq::moqx
