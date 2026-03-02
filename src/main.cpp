#include <o_rly/ORelayServer.h>
#include <o_rly/config/loader.h>

#include <folly/Expected.h>
#include <folly/init/Init.h>

#include <iostream>
#include <string>

DEFINE_string(config, "", "Path to config file (required)");
DEFINE_bool(strict_config, false, "Reject unknown config fields");

namespace {

// On success returns LoadResult for "serve" mode.
// On error returns an exit code (for early-exit subcommands or failures).
folly::Expected<openmoq::o_rly::config::LoadResult, int> handleSubcommand(int argc, char* argv[]) {
  std::string subcommand = "serve";
  if (argc > 1) {
    subcommand = argv[1];
  }

  if (subcommand == "dump-config-schema") {
    std::cout << openmoq::o_rly::config::generateSchema() << '\n';
    return folly::makeUnexpected(0);
  }

  if (subcommand != "serve" && subcommand != "validate-config") {
    std::cerr << "Unknown subcommand: " << subcommand << "\n";
    google::ShowUsageWithFlags(argv[0]);
    return folly::makeUnexpected(1);
  }

  if (FLAGS_config.empty()) {
    std::cerr << "Error: --config is required\n";
    google::ShowUsageWithFlags(argv[0]);
    return folly::makeUnexpected(1);
  }

  openmoq::o_rly::config::LoadResult lr;
  try {
    lr = openmoq::o_rly::config::loadConfig(FLAGS_config, FLAGS_strict_config);
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return folly::makeUnexpected(1);
  }

  for (const auto& w : lr.warnings) {
    std::cerr << "Warning: " << w << std::endl;
  }

  if (subcommand == "validate-config") {
    std::cout << "Config is valid." << '\n';
    return folly::makeUnexpected(0);
  }

  return lr;
}

std::shared_ptr<openmoq::o_rly::ORelayServer>
createServer(const openmoq::o_rly::config::Config& cfg) {
  const auto& listener = cfg.listeners.value()[0];
  auto cache = cfg.cacheOrDefault();

  size_t maxCachedTracks =
      cache.enabledOrDefault() ? static_cast<size_t>(cache.maxTracksOrDefault()) : 0;
  size_t maxCachedGroupsPerTrack = static_cast<size_t>(cache.maxGroupsPerTrackOrDefault());

  std::string versions = openmoq::o_rly::config::moqtVersionsToString(listener);
  std::string endpoint = listener.endpointOrDefault();

  const auto& tlsOpt = listener.tls_credentials.value();
  if (!tlsOpt.has_value() || tlsOpt->insecureOrDefault()) {
    return std::make_shared<openmoq::o_rly::ORelayServer>(endpoint, versions, maxCachedTracks,
                                                          maxCachedGroupsPerTrack);
  }
  const auto& tls = *tlsOpt;
  return std::make_shared<openmoq::o_rly::ORelayServer>(
      tls.cert_file.value().value_or(""), tls.key_file.value().value_or(""), endpoint, versions,
      maxCachedTracks, maxCachedGroupsPerTrack);
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
  auto& lr = result.value();

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
  auto server = createServer(lr.config);

  // === 7. Start health checks / admin endpoints ===
  // TODO: readiness/liveness probes
  // Health state machine: starting → healthy → draining → stopped

  // === 8. Start serving ===
  // Bind listeners, accept connections, enter event loop
  const auto& listener = lr.config.listeners.value()[0];
  auto sock = listener.udp.value()->socketOrDefault();

  folly::SocketAddress addr(sock.addressOrDefault(), sock.portOrDefault());
  server->start(addr);
  evb.loopForever();

  // ============================================
  // Teardown (reverse order, on signal/shutdown)
  // ============================================

  // === 9. Stop accepting new connections ===
  // TODO: close listeners

  // === 10. Drain in-flight requests ===
  // TODO: wait with timeout for active work to finish
  // Timeout on drain — don't wait forever for graceful shutdown

  // === 11. Shut down dependencies ===
  // TODO: TBD

  // === 12. Flush telemetry/logs ===
  // TODO: ensure observability data is sent

  // === 13. Clean up resources ===
  // TODO: TBD

  // === 14. Exit with appropriate code ===
  return 0;
}
