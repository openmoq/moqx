#pragma once

#include <memory>

namespace openmoq::moqx {
class MoqxRelayServer;
} // namespace openmoq::moqx

namespace openmoq::moqx::admin {

class AdminServer;

// Registers POST /cache-purge on the given admin server.
//
// Clears all service caches, or a single named service when the
// ?service=<name> query parameter is provided. Responds with JSON:
//   {"status":"ok","cleared":<N>}
// or 404 if the named service is not found.
void registerCachePurgeRoute(AdminServer& server, std::shared_ptr<MoqxRelayServer> relayServer);

} // namespace openmoq::moqx::admin
