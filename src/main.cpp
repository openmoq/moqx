#include <o_rly/ORelayServer.h>
#include <o_rly/admin/AdminServer.h>
#include <o_rly/admin/BuiltinRoutes.h>

#include <folly/init/Init.h>
#include <folly/logging/xlog.h>

DEFINE_string(cert, "", "Cert path");
DEFINE_string(key, "", "Key path");
DEFINE_string(endpoint, "/moq-relay", "End point");
DEFINE_int32(port, 9668, "Relay Server Port");
DEFINE_bool(enable_cache, true, "Enable relay cache");
DEFINE_bool(insecure, false, "Use insecure verifier (skip certificate validation)");
DEFINE_string(
    versions,
    "",
    "Comma-separated MoQ draft versions (e.g. \"14,16\"). Empty = all "
    "supported."
);
DEFINE_int32(max_cached_tracks, 100, "Maximum number of cached tracks (0 to disable caching)");
DEFINE_int32(max_cached_groups_per_track, 3, "Maximum groups per track in cache");
DEFINE_int32(admin_port, 9669, "HTTP admin port");

int main(int argc, char* argv[]) {

  // === 1. Parse command-line flags/config ===
  // CLI args, config files, env vars
  folly::Init init(&argc, &argv, true);

  size_t maxCachedTracks = FLAGS_enable_cache ? static_cast<size_t>(FLAGS_max_cached_tracks) : 0;
  size_t maxCachedGroupsPerTrack = static_cast<size_t>(FLAGS_max_cached_groups_per_track);

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
  std::shared_ptr<openmoq::o_rly::ORelayServer> server = nullptr;
  if (FLAGS_insecure) {
    server = std::make_shared<openmoq::o_rly::ORelayServer>(
        FLAGS_endpoint,
        FLAGS_versions,
        maxCachedTracks,
        maxCachedGroupsPerTrack
    );
  } else {
    server = std::make_shared<openmoq::o_rly::ORelayServer>(
        FLAGS_cert,
        FLAGS_key,
        FLAGS_endpoint,
        FLAGS_versions,
        maxCachedTracks,
        maxCachedGroupsPerTrack
    );
  }

  // === 7. Start health checks / admin endpoints ===
  openmoq::o_rly::admin::AdminServer adminServer;
  openmoq::o_rly::admin::registerBuiltinRoutes(adminServer);
  if (!adminServer.start(static_cast<uint16_t>(FLAGS_admin_port))) {
    XLOG(FATAL) << "Failed to start admin server on port " << FLAGS_admin_port;
  }
  XLOG(INFO) << "Admin server listening on port " << FLAGS_admin_port;

  // === 8. Start serving ===
  // Bind listeners, accept connections, enter event loop
  folly::SocketAddress addr("::", FLAGS_port);
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
  // Stop admin last — allows a final metrics scrape during drain.
  adminServer.stop();

  // === 14. Exit with appropriate code ===
  return 0;
}
