/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <string>

#include <folly/Expected.h>

#include "config/Config.h"
#include "logging/access/MoqxAccessLog.h"

namespace openmoq::moqx::logging {

// Builds the access log from the resolved config. Without a configured sink, the log is
// disabled. Fails when a sink's file cannot be opened. The access log is not started.
folly::Expected<std::shared_ptr<MoqxAccessLog>, std::string>
makeAccessLog(const config::Config& config);

// The record filter for one sink's events list: class names expand to their events, and
// an empty list takes every event.
AccessLogSinkFilter makeAccessLogSinkFilter(const config::AccessLogSinkConfig& sink);

} // namespace openmoq::moqx::logging
