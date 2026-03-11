#pragma once

#include <memory>

#include <folly/Executor.h>

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
// The handler:
//   1. Schedules StatsRegistry::aggregateAsync() on relayExecutor.
//   2. Formats the result as Prometheus exposition text and sends the response
//      asynchronously (does not block the admin thread).
void registerMetricsRoute(
    AdminServer& adminServer,
    std::shared_ptr<stats::StatsRegistry> registry,
    folly::Executor::KeepAlive<> relayExecutor);

} // namespace admin

} // namespace openmoq::o_rly
