#pragma once

#include <memory>

namespace openmoq::o_rly {

namespace admin {
class AdminServer;
}

namespace stats {
class StatsRegistry;
}

namespace admin {

// Registers GET /metrics on the given admin server.
//
// The handler aggregates stats asynchronously using co_withCancellation so
// that a client disconnect (onError) propagates cancellation into
// collectAllRange and the completion lambda becomes a no-op instead of
// trying to write to an already-closed downstream.
void registerMetricsRoute(AdminServer& adminServer, std::shared_ptr<stats::StatsRegistry> registry);

} // namespace admin

} // namespace openmoq::o_rly
