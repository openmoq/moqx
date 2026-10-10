/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <string>

#include "logging/QLogCapture.h"

namespace openmoq::moqx::admin {

class AdminServer;

// Registers the on-demand qlog capture routes (mvfst listeners only). Each
// responds 503 when logging.qlog.dir is not configured (capture is null).
//
// POST /qlog/capture?count=N&seconds=S&mode=cc|full&replace=1
//   qlogs the next N new connections (default 1, max 64) arriving within S
//   seconds (default 60, max 600). mode=cc (default) drops per-packet and
//   per-stream events. 409 if a capture is in progress, unless replace=1.
// DELETE /qlog/capture
//   Disarms.
// GET /qlog/capture
//   Capture status and the newest 100 qlog files in the directory, each
//   fetchable with GET /logs?connection_id=<id>&type=qlog. Scans at most
//   10000 entries; "truncated" says the scan stopped early.
void registerQLogCaptureRoutes(
    AdminServer& adminServer,
    std::shared_ptr<logging::QLogCapture> capture,
    std::string qlogDir
);

} // namespace openmoq::moqx::admin
