#include <moqx/MoqxRelayServer.h>
#include <moqx/admin/AdminServer.h>
#include <moqx/admin/BuiltinRoutes.h>
#include <moqx/admin/CachePurgeHandler.h>
#include <moqx/admin/MetricsHandler.h>
#include <moqx/config/loader/config_init.h>
#include <moqx/stats/StatsRegistry.h>

#include <csignal>

#include <chrono>
#include <thread>

#include <folly/init/Init.h>
#include <folly/io/async/AsyncSignalHandler.h>
#include <folly/logging/xlog.h>

#include <iostream>
#include <string_view>

DEFINE_string(config, "", "Path to config file (required)");
DEFINE_bool(strict_config, false, "Reject unknown config fields");

namespace {

namespace cfg = openmoq::moqx::config;

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

std::shared_ptr<openmoq::moqx::MoqxRelayServer> createServer(const cfg::Config& config) {
  const auto& listener = config.listener;
  auto services = config.services; // copy for move into constructor

  return std::visit(
      [&](const auto& tls) -> std::shared_ptr<openmoq::moqx::MoqxRelayServer> {
        using T = std::decay_t<decltype(tls)>;
        if constexpr (std::is_same_v<T, cfg::Insecure>) {
          return std::make_shared<openmoq::moqx::MoqxRelayServer>(
              listener.endpoint,
              listener.moqtVersions,
              std::move(services),
              config.relayID
          );
        } else {
          return std::make_shared<openmoq::moqx::MoqxRelayServer>(
              tls.certFile,
              tls.keyFile,
              listener.endpoint,
              listener.moqtVersions,
              std::move(services),
              config.relayID
          );
        }
      },
      listener.tlsMode
  );
}

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
  // TODO: logging framework, log levels, structured logging
  // (currently handled implicitly by folly::Init)

  // === 3. Set up signal handling ===
  folly::EventBase evb;
  ShutdownSignalHandler signalHandler(&evb);

  // === 4. Initialize resources ===
  // TODO: thread pools, event loops, IO contexts

  // === 5. Initialize dependencies ===
  // TODO: TBD

  // === 6. Initialize services ===
  // Construct and configure the application's own services
  // (MoqxRelayServer, MoqxRelay, etc.)
  auto server = createServer(config);

  // === 6a. Stats registry ===
  auto statsRegistry = std::make_shared<openmoq::moqx::stats::StatsRegistry>();
  server->setStatsRegistry(statsRegistry);

  // === 7. Start health checks / admin endpoints ===
  openmoq::moqx::admin::AdminServer adminServer;
  openmoq::moqx::admin::registerBuiltinRoutes(adminServer);
  openmoq::moqx::admin::registerMetricsRoute(adminServer, statsRegistry);
  openmoq::moqx::admin::registerCachePurgeRoute(adminServer, server);
  if (config.admin) {
    if (!adminServer.start(*config.admin)) {
      XLOG(FATAL) << "Failed to start admin server on " << config.admin->address.describe();
    }

    XLOG(INFO) << "Admin server listening on " << config.admin->address.describe();
  }

  // === 8. Start serving ===
  server->start(config.listener.address);
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
  // TODO: close listeners

  // === 10. Drain in-flight requests ===
  // TODO: wait with timeout for active work to finish
  // Timeout on drain -- don't wait forever for graceful shutdown

  // === 11. Shut down dependencies ===
  // TODO: TBD

  // === 12. Flush telemetry/logs ===
  // TODO: ensure observability data is sent

  // === 13. Clean up resources ===
  // Stop admin last — allows a final metrics scrape during drain.
  adminServer.stop();

  // === 14. Exit with appropriate code ===
  return 0;
}
