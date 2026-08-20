/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <memory>

#include <folly/io/IOBuf.h>

#include "MoqxRelayContext.h"

namespace openmoq::moqx {

namespace admin {
class AdminServer;

struct TrackMetricsLimits {
  bool enabled{true};
  size_t defaultLimit{10};
  size_t maxLimit{1000};
};

std::unique_ptr<folly::IOBuf>
formatTrackMetrics(const MoqxRelayContext::TrackMetricsResult& result, bool omitMetadata = false);

// Registers GET /metrics/track, which reports per-track counters in Prometheus
// text format for tracks matching ?service=&namespace=&track=.
void registerTrackMetricsRoute(
    AdminServer& adminServer,
    std::shared_ptr<MoqxRelayContext> context,
    TrackMetricsLimits limits = {}
);

} // namespace admin

} // namespace openmoq::moqx
