#include <o_rly/ORelayServer.h>
#include <o_rly/config/config_resolver.h>
#include <o_rly/config/loader.h>

#include <folly/Expected.h>
#include <folly/init/Init.h>

#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

DEFINE_string(config, "", "Path to config file (required)");
DEFINE_bool(strict_config, false, "Reject unknown config fields");

namespace {

namespace cfg = openmoq::o_rly::config;

constexpr std::string_view kServeCommand = "serve";
constexpr std::string_view kValidateConfigCommand = "validate-config";
constexpr std::string_view kDumpConfigSchemaCommand = "dump-config-schema";

// On success returns resolved Config for "serve" mode.
// On error returns an exit code (for early-exit subcommands or failures).
folly::Expected<cfg::Config, int> handleSubcommand(int argc, char* argv[]) {
  std::string_view subcommand = kServeCommand;
  if (argc > 1) {
    subcommand = argv[1];
  }

  if (subcommand == kDumpConfigSchemaCommand) {
    std::cout << cfg::generateSchema() << '\n';
    return folly::makeUnexpected(0);
  }

  if (subcommand != kServeCommand && subcommand != kValidateConfigCommand) {
    std::cerr << "Unknown subcommand: " << subcommand << "\n";
    google::ShowUsageWithFlags(argv[0]);
    return folly::makeUnexpected(1);
  }

  if (FLAGS_config.empty()) {
    std::cerr << "Error: --config is required\n";
    google::ShowUsageWithFlags(argv[0]);
    return folly::makeUnexpected(1);
  }

  cfg::ParsedConfig parsed;
  try {
    parsed = cfg::loadConfig(FLAGS_config, FLAGS_strict_config);
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return folly::makeUnexpected(1);
  }

  auto diag = cfg::diagnoseConfig(parsed);
  for (const auto& w : diag.warnings) {
    std::cerr << "Warning: " << w << std::endl;
  }
  if (!diag.errors.empty()) {
    std::ostringstream oss;
    oss << "Config validation failed:";
    for (const auto& err : diag.errors) {
      oss << "\n  - " << err;
    }
    std::cerr << "Error: " << oss.str() << std::endl;
    return folly::makeUnexpected(1);
  }

  if (subcommand == kValidateConfigCommand) {
    std::cout << "Config is valid." << '\n';
    return folly::makeUnexpected(0);
  }

  return cfg::resolveConfig(parsed);
}

std::shared_ptr<openmoq::o_rly::ORelayServer> createServer(const cfg::Config& resolved) {
  const auto& listener = resolved.listener;
  const auto& cache = resolved.cache;

  return std::visit(
      [&](const auto& tls) -> std::shared_ptr<openmoq::o_rly::ORelayServer> {
        using T = std::decay_t<decltype(tls)>;
        if constexpr (std::is_same_v<T, cfg::Insecure>) {
          return std::make_shared<openmoq::o_rly::ORelayServer>(
              listener.endpoint,
              listener.moqtVersions,
              cache.maxCachedTracks,
              cache.maxCachedGroupsPerTrack
          );
        } else {
          return std::make_shared<openmoq::o_rly::ORelayServer>(
              tls.certFile,
              tls.keyFile,
              listener.endpoint,
              listener.moqtVersions,
              cache.maxCachedTracks,
              cache.maxCachedGroupsPerTrack
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
  google::SetUsageMessage("o-rly MoQ relay server\n\n"
                          "Subcommands:\n"
                          "  serve                Start the relay (default)\n"
                          "  validate-config      Load and validate config, then exit\n"
                          "  dump-config-schema   Print JSON schema to stdout, then exit\n\n"
                          "Usage: o_rly [subcommand] --config <path>");
  folly::Init init(&argc, &argv, true);

  auto result = handleSubcommand(argc, argv);
  if (result.hasError()) {
    return result.error();
  }
  const auto& cfg = result.value();

  // === 2. Set up logging/observability ===
  // TODO: logging framework, log levels, structured logging
  // (currently handled implicitly by folly::Init)

  // === 3. Set up signal handling ===
  // TODO: SIGINT, SIGTERM handlers for graceful shutdown
  // Use a shutdown promise/future triggered by signals

  // === 4. Initialize resources ===
  // TODO: thread pools, event loops, IO contexts
  folly::EventBase evb;

  // === 5. Initialize dependencies ===
  // TODO: TBD

  // === 6. Initialize services ===
  // Construct and configure the application's own services
  // (ORelayServer, ORelay, etc.)
  auto server = createServer(cfg);

  // === 7. Start health checks / admin endpoints ===
  // TODO: readiness/liveness probes
  // Health state machine: starting -> healthy -> draining -> stopped

  // === 8. Start serving ===
  // Bind listeners, accept connections, enter event loop
  server->start(cfg.listener.address);
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
  // TODO: TBD

  // === 14. Exit with appropriate code ===
  return 0;
}
