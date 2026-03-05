#pragma once

namespace openmoq::o_rly::admin {

class AdminServer;

// Registers built-in admin routes: GET /info, and future /health, /version.
void registerBuiltinRoutes(AdminServer& server);

} // namespace openmoq::o_rly::admin
