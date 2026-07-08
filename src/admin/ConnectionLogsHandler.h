/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <optional>

#include "config/Config.h"

namespace openmoq::moqx::admin {

class AdminServer;

// Registers GET /logs on the admin server.
//
// GET /logs?connection_id=<hex>&type=mlog|qlog
//   Resolves the file path as {log_dir}/{normalized_cid}.{ext} and streams
//   the file directly. No index or disk scan is required — files written
//   after startup are immediately reachable.
//   Responds 400 for missing/invalid params, 503 if the requested type is
//   not configured, 404 if the file does not exist.
void registerConnectionLogsRoutes(
    AdminServer& adminServer,
    const std::optional<config::LoggingConfig>& logging
);

} // namespace openmoq::moqx::admin
