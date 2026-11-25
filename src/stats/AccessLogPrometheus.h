/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>

#include <folly/coro/Task.h>
#include <folly/io/IOBuf.h>

#include "logging/access/MoqxAccessLog.h"

namespace openmoq::moqx::stats {

// The moqx_access_log_* series in Prometheus text.
folly::coro::Task<std::unique_ptr<folly::IOBuf>>
accessLogPrometheusText(logging::MoqxAccessLog& accessLog, bool omitMetadata);

} // namespace openmoq::moqx::stats
