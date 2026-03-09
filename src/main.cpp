#include <o_rly/ORelayServer.h>
#include <o_rly/admin/AdminServer.h>
#include <o_rly/admin/BuiltinRoutes.h>
#include <o_rly/config/loader/config_init.h>
#include <o_rly/tls/builtin_tls_providers.h>
#include <o_rly/tls/fizz_context_factory.h>
#include <o_rly/tls/tls_cert_loader.h>
#include <o_rly/tls/tls_provider_registry.h>

#include <csignal>

#include <folly/init/Init.h>
#include <folly/io/async/AsyncSignalHandler.h>
#include <folly/logging/xlog.h>

#include <iostream>
#include <string_view>

DEFINE_string(config, "", "Path to config file (required)");
DEFINE_bool(strict_config, false, "Reject unknown config fields");

namespace {

namespace cfg = openmoq::o_rly::config;

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

std::shared_ptr<openmoq::o_rly::ORelayServer> createServer(const cfg::Config& resolved) {
  const auto& listener = resolved.listener;
  const auto& cache = resolved.cache;

  auto alpns = openmoq::o_rly::tls::buildAlpns(listener.moqtVersions);
  auto fizzCtx = listener.tlsProvider->createContext(alpns);
  if (fizzCtx.hasError()) {
    XLOG(FATAL) << "Failed to create TLS context: " << fizzCtx.error();
  }

  return std::make_shared<openmoq::o_rly::ORelayServer>(
      std::move(fizzCtx.value()),
      listener.endpoint,
      cache.maxCachedTracks,
      cache.maxCachedGroupsPerTrack
  );
}

} // namespace

int main(int argc, char* argv[]) {

  // === 1. Parse command-line flags/config ===
  // CLI args, config files, env vars
  google::SetUsageMessage(
      "o-rly MoQ relay server\n\n"
      "Subcommands:\n"
      "  serve                Start the relay (default)\n" +
      cfg::configSubcommandUsage() + "\nUsage: o_rly [subcommand] --config <path>"
  );
  folly::Init init(&argc, &argv, true);

  std::string_view subcommand = kServeCommand;
  if (argc > 1) {
    subcommand = argv[1];
  }

  // Create TLS provider registry and register built-in providers
  openmoq::o_rly::tls::TlsProviderRegistry tlsRegistry;
  openmoq::o_rly::tls::registerBuiltinTlsProviders(tlsRegistry);

  auto result = cfg::handleConfigSubcommand(
      subcommand,
      FLAGS_config,
      FLAGS_strict_config,
      argv[0],
      tlsRegistry
  );
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
  // (ORelayServer, ORelay, etc.)
  auto server = createServer(config);

  // === 7. Start health checks / admin endpoints ===
  openmoq::o_rly::admin::AdminServer adminServer;
  openmoq::o_rly::admin::registerBuiltinRoutes(adminServer);
  if (config.admin) {
    if (!adminServer.start(*config.admin)) {
      XLOG(FATAL) << "Failed to start admin server on " << config.admin->address.describe();
    }
    XLOG(INFO) << "Admin server listening on " << config.admin->address.describe();
  }

  // === 8. Start serving ===
  // Bind listeners, accept connections, enter event loop
  server->start(config.listener.address);
  evb.loopForever();

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
