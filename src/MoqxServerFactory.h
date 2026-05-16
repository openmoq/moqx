/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <string>

#include "MoqxPicoRelayServer.h"
#include "MoqxRelayContext.h"
#include "MoqxRelayServer.h"
#include "config/Config.h"
#include "stats/StatsRegistry.h"
#include <folly/executors/IOThreadPoolExecutor.h>
#include <moxygen/MoQServerBase.h>
#include <moxygen/mlog/MLoggerFactory.h>

namespace openmoq::moqx {

// Creates the appropriate relay server for the given listener config and wires
// stats. Both stack paths accept the same ioExecutor for a uniform call site.
// Optionally wires an mlog factory for per-session logging.
inline std::shared_ptr<moxygen::MoQServerBase> makeRelayServer(
    const config::ListenerConfig& listenerCfg,
    std::shared_ptr<MoqxRelayContext> context,
    std::shared_ptr<folly::IOThreadPoolExecutor> ioExecutor,
    std::shared_ptr<stats::StatsRegistry> statsRegistry,
    std::shared_ptr<moxygen::MLoggerFactory> mlogFactory = nullptr
) {
  if (listenerCfg.quicStack == config::QuicStack::Picoquic) {
    auto server = std::make_shared<MoqxPicoRelayServer>(
        listenerCfg,
        std::move(context),
        std::move(ioExecutor)
    );
    server->setStatsRegistry(std::move(statsRegistry));
    if (mlogFactory) {
      server->setMLoggerFactory(std::move(mlogFactory));
    }
    return server;
  }
  auto server =
      std::make_shared<MoqxRelayServer>(listenerCfg, std::move(context), std::move(ioExecutor));
  server->setStatsRegistry(std::move(statsRegistry));
  if (mlogFactory) {
    server->setMLoggerFactory(std::move(mlogFactory));
  }
  return server;
}

} // namespace openmoq::moqx
