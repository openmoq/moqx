#pragma once

namespace openmoq::moqx::admin {

class AdminServer;

// Registers built-in admin routes: GET /info, and future /health, /version.
void registerBuiltinRoutes(AdminServer& server);

} // namespace openmoq::moqx::admin
