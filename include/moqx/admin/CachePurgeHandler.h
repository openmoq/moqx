#pragma once

#include <memory>

namespace openmoq::moqx {
class MoqxRelayContext;
} // namespace openmoq::moqx

namespace openmoq::moqx::admin {

class AdminServer;

// Registers POST /cache-purge on the given admin server.
//
// Clears all service caches, or a single named service when the
// ?service=<name> query parameter is provided. Responds with JSON:
//   {"status":"ok","cleared":<N>}
// or 404 if the named service is not found.
void registerCachePurgeRoute(AdminServer& adminServer, std::shared_ptr<MoqxRelayContext> context);

} // namespace openmoq::moqx::admin
