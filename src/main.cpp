/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MoqxRelayContext.h"
#include "MoqxServerFactory.h"
#include "admin/AdminServer.h"
#include "admin/BuiltinRoutes.h"
#include "admin/CachePurgeHandler.h"
#include "admin/MetricsHandler.h"
#include "admin/StateHandler.h"
#include "config/loader/ConfigInit.h"
#include "stats/StatsRegistry.h"
#include "logging/MLogCleaner.h"

#include <csignal>

#include <chrono>
#include <thread>

#include <folly/executors/IOThreadPoolExecutor.h>
#include <folly/executors/thread_factory/NamedThreadFactory.h>
#include <folly/init/Init.h>
#include <folly/io/async/AsyncSignalHandler.h>
#include <folly/logging/xlog.h>
#include <moxygen/mlog/FileMLoggerFactory.h>
#include <moxygen/mlog/SamplingMLoggerFactory.h>

#include <iostream>
#include <string_view>

DEFINE_string(config, "", "Path to config file (required)");
DEFINE_bool(strict_config, false, "Reject unknown config fields");

using namespace openmoq::moqx;

namespace {

namespace cfg = config;

constexpr std::string_view kServeCommand = "serve";

class ShutdownSignalHandler : public folly::AsyncSignalHandler {
public:
  explicit ShutdownSignalHandler(folly::EventBase* evb) : AsyncSignalHandler(evb) {
    registerSignalHandler(SIGTERM);
    registerSignalHandler(SIGINT);
  }

  void signalReceived(int signum) noexcept override {
    XLOG(INFO) << "Received signal " << signum << ", shutting down";
    getEventBase()->terminateLoopSoon();
  }
};

} // namespace

int main(int argc, char* argv[]) {

  // === 1. Parse command-line flags/config ===
  // CLI args, config files, env vars
  google::SetUsageMessage(
      "moqx MoQ relay server\n\n"
      "Subcommands:\n"
      "  serve                Start the relay (default)\n" +
      cfg::configSubcommandUsage() + "\nUsage: moqx [subcommand] --config <path>"
  );
  folly::Init init(&argc, &argv, true);

  std::string_view subcommand = kServeCommand;
  if (argc > 1) {
    subcommand = argv[1];
  }

  auto result = cfg::handleConfigSubcommand(subcommand, FLAGS_config, FLAGS_strict_config, argv[0]);
  if (result.hasError()) {
    return result.error();
  }

  for (const auto& w : result.value().warnings) {
    XLOG(WARNING) << w;
  }
  const auto& config = result.value().config;

  if (subcommand != kServeCommand) {
    std::cerr << "Unknown subcommand: " << subcommand << "\n";
    google::ShowUsageWithFlags(argv[0]);
    return 1;
  }

  // === 2. Set up logging/observability ===
  std::shared_ptr<moxygen::MLoggerFactory> mlogFactory;
  std::shared_ptr<folly::IOThreadPoolExecutor> mlogExecutor;
  std::shared_ptr<logging::MLogCleaner> mlogCleaner;
  // Shared_ptr to a recursive schedule function; kept alive until evb exits.
  std::shared_ptr<std::function<void()>> mlogCleanupSchedule;

  if (config.logging && config.logging->mlog) {
    const auto& mcfg = *config.logging->mlog;
    auto baseFactory = std::make_shared<moxygen::FileMLoggerFactory>(moxygen::VantagePoint::SERVER);
    // Set output directory on the underlying factory
    if (!mcfg.dir.empty()) {
      baseFactory->setDir(mcfg.dir);
    }
    // Use background executor for mlog writes
    mlogExecutor = std::make_shared<folly::IOThreadPoolExecutor>(
        1,
        std::make_shared<folly::NamedThreadFactory>("moqx-mlog")
    );
    baseFactory->setWriteExecutor(mlogExecutor);
    if (mcfg.sampleRate < 1.0f) {
      mlogFactory = std::make_shared<moxygen::SamplingMLoggerFactory>(baseFactory, mcfg.sampleRate);
    } else {
      mlogFactory = std::move(baseFactory);
    }

    // Set up directory cleanup if either retention limit is configured.
    if (!mcfg.dir.empty() && (mcfg.maxAgeDays || mcfg.maxDirMb)) {
      mlogCleaner = std::make_shared<logging::MLogCleaner>(
          mcfg.dir, mcfg.maxAgeDays, mcfg.maxDirMb
      );
    }
  }

  // === 3. Set up signal handling ===
  folly::EventBase evb;
  ShutdownSignalHandler signalHandler(&evb);

  // === 4. Initialize resources ===
  auto ioExecutor = std::make_shared<folly::IOThreadPoolExecutor>(
      config.threads,
      std::make_shared<folly::NamedThreadFactory>("moqx-io")
  );

  // === 5. Initialize dependencies ===
  // TODO: TBD

  // === 6. Initialize services ===
  // Construct and configure the application's own services
  // (MoqxRelayContext, MoqxRelayServer, etc.)
  auto context = std::make_shared<MoqxRelayContext>(config.services, config.relayID);

  // === 6a. Stats registry ===
  auto statsRegistry = std::make_shared<stats::StatsRegistry>();

  std::vector<std::shared_ptr<moxygen::MoQServerBase>> servers;
  for (const auto& listenerCfg : config.listeners) {
    servers.emplace_back(
        makeRelayServer(listenerCfg, context, ioExecutor, statsRegistry, mlogFactory)
    );
  }

  if (!servers.empty()) {
    context->setCacheEvb(ioExecutor->getAllEventBases()[0].get());
  }

  // === 7. Start health checks / admin endpoints ===
  admin::AdminServer adminServer;
  admin::registerBuiltinRoutes(adminServer);
  admin::registerMetricsRoute(adminServer, statsRegistry);
  admin::registerCachePurgeRoute(adminServer, context);
  admin::registerStateRoute(adminServer, context);
  if (config.admin) {
    if (!adminServer.start(*config.admin)) {
      XLOG(FATAL) << "Failed to start admin server on " << config.admin->address.describe();
    }

    XLOG(INFO) << "Admin server listening on " << config.admin->address.describe();
  }

  // === 8. Start serving ===
  for (auto& server : servers) {
    // addr ignored — each server binds its own listenerCfg address
    server->start(folly::SocketAddress{});
  }

  if (!servers.empty()) {
    context->initUpstreams(ioExecutor->getAllEventBases()[0].get());
  }

  // Schedule mlog directory cleanup if configured.
  // Runs at startup (to clean files from previous runs) and then periodically
  // so the directory doesn't grow unbounded
  if (mlogCleaner && mlogExecutor) {
    const uint32_t intervalMs = config.logging->mlog->cleanupIntervalSecs * 1000;
    // Startup pass — run immediately on the mlog executor.
    mlogExecutor->add([c = mlogCleaner] { c->cleanup(); });
    // Periodic pass — schedule via the main event base, post work to executor.
    mlogCleanupSchedule = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weak = mlogCleanupSchedule;
    *mlogCleanupSchedule =
        [&evb, c = mlogCleaner, exec = mlogExecutor, weak, intervalMs]() {
          evb.runAfterDelay(
              [c, exec, weak, intervalMs]() {
                exec->add([c] { c->cleanup(); });
                //self-reschedule for the next run, if still alive
                if (auto fn = weak.lock()) {
                  (*fn)();
                }
              },
              intervalMs);
        };
    (*mlogCleanupSchedule)();
  }

  evb.loopForever();

  // Hard shutdown watchdog: if teardown hangs, force-exit after 10 seconds.
  std::thread([relayID = config.relayID] {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    XLOG(ERR) << "Shutdown timed out after 10s (relay_id=" << relayID << "), forcing exit";
    std::_Exit(1);
  }).detach();

  // ============================================
  // Teardown (reverse order, on signal/shutdown)
  // ============================================

  // === 9. Stop accepting new connections ===
  // Signal upstreams to stop — cancels reconnect backoff so worker EVBs can
  // exit cleanly before servers drain them.
  context->stop();
  // TODO: close listeners

  // === 10. Drain in-flight requests ===
  // TODO: wait with timeout for active work to finish
  // Timeout on drain -- don't wait forever for graceful shutdown

  // === 11. Shut down dependencies ===
  // TODO: TBD

  // === 12. Flush telemetry/logs ===
  // Join mlog write executor after all sessions have closed so that any
  // pending outputLogs() tasks complete before process exit.
  if (mlogExecutor) {
    mlogExecutor->join();
  }

  // === 13. Clean up resources ===
  // Stop admin last — allows a final metrics scrape during drain.
  adminServer.stop();

  // === 14. Exit with appropriate code ===
  return 0;
}
