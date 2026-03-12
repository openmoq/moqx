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
// The handler aggregates stats and formats the result as Prometheus exposition
// text. Uses blockingWait to handle aggregation synchronously on the admin
// thread
void registerMetricsRoute(AdminServer& adminServer, std::shared_ptr<stats::StatsRegistry> registry);

} // namespace admin

} // namespace openmoq::o_rly
