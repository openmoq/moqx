#pragma once

#include <memory>

namespace openmoq::moqx {
class MoqxRelayContext;
} // namespace openmoq::moqx

namespace openmoq::moqx::admin {

class AdminServer;

// Registers POST /cache/purge on the given admin server.
//
// Accepts an optional JSON body:
//   {"service":"...","namespace":"...","track":"..."}
// All fields are optional; omitting all purges every evictable track across
// all services. Responds with:
//   {"evicted":<N>,"skipped":<M>}
void registerCachePurgeRoute(AdminServer& adminServer, std::shared_ptr<MoqxRelayContext> context);

} // namespace openmoq::moqx::admin
