/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>

#include "MoqxPicoRelayServer.h"
#include "MoqxRelayContext.h"
#include "MoqxRelayServer.h"
#include "config/Config.h"
#include "stats/StatsRegistry.h"
#include "tls/FizzContextFactory.h"
#include "tls/TlsCertLoader.h"
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
    auto server = std::make_shared<MoqxPicoRelayServer>(
        listenerCfg,
        std::move(context),
        std::move(ioExecutor)
    );
    server->setStatsRegistry(std::move(statsRegistry));
    return server;
  }

  auto alpns = openmoq::moqx::tls::buildAlpns(listenerCfg.moqtVersions);
  auto fizzCtx = listenerCfg.tlsProvider->createContext(alpns);
  if (fizzCtx.hasError()) {
    XLOG(FATAL) << "Failed to create TLS context: " << fizzCtx.error();
  }

  auto server = std::make_shared<MoqxRelayServer>(
      listenerCfg,
      std::move(context),
      std::move(fizzCtx).value(),
      std::move(ioExecutor)
  );
  server->setStatsRegistry(std::move(statsRegistry));
  return server;
}

} // namespace openmoq::moqx
