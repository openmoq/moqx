#include <o_rly/ORelayServer.h>

#include <folly/init/Init.h>

using namespace proxygen;

DEFINE_string(cert, "", "Cert path");
DEFINE_string(key, "", "Key path");
DEFINE_string(endpoint, "/moq-relay", "End point");
DEFINE_int32(port, 9668, "Relay Server Port");
DEFINE_bool(enable_cache, true, "Enable relay cache");
DEFINE_bool(
    insecure,
    false,
    "Use insecure verifier (skip certificate validation)");
DEFINE_string(
    versions,
    "",
    "Comma-separated MoQ draft versions (e.g. \"14,16\"). Empty = all "
    "supported.");
DEFINE_int32(
    max_cached_tracks,
    100,
    "Maximum number of cached tracks (0 to disable caching)");
DEFINE_int32(
    max_cached_groups_per_track,
    3,
    "Maximum groups per track in cache");

int main(int argc, char* argv[]) {
  folly::Init init(&argc, &argv, true);

  size_t maxCachedTracks =
      FLAGS_enable_cache ? static_cast<size_t>(FLAGS_max_cached_tracks) : 0;
  size_t maxCachedGroupsPerTrack =
      static_cast<size_t>(FLAGS_max_cached_groups_per_track);

  std::shared_ptr<openmoq::o_rly::ORelayServer> server = nullptr;
  if (FLAGS_insecure) {
    server = std::make_shared<openmoq::o_rly::ORelayServer>(
        FLAGS_endpoint,
        FLAGS_versions,
        maxCachedTracks,
        maxCachedGroupsPerTrack);
  } else {
    server = std::make_shared<openmoq::o_rly::ORelayServer>(
        FLAGS_cert,
        FLAGS_key,
        FLAGS_endpoint,
        FLAGS_versions,
        maxCachedTracks,
        maxCachedGroupsPerTrack);
  }

  folly::SocketAddress addr("::", FLAGS_port);
  server->start(addr);
  folly::EventBase evb;
  evb.loopForever();
  return 0;
}
