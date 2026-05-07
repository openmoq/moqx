/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>

namespace openmoq::moqx {

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

} // namespace openmoq::moqx
