/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <folly/io/async/EventBase.h>

#include "stats/BoundedHistogram.h"
#include "stats/StatsRegistry.h"

namespace openmoq::moqx::stats {

/**
 * Collects folly EventBase loop-time statistics for one IO thread.
 *
 * Implements folly::EventBaseObserver (samples every loop iteration) and
 * StatsCollectorBase (registered with the StatsRegistry for aggregation).
 *
 * Other collectors may subscribe to loop samples via addLoopObserver().
 * Subscribers MUST outlive this collector (i.e. outlive the EventBase).
 *
 * Ownership: shared_ptr held by both the StatsRegistry and the EventBase
 * observer slot.  Call create() once per worker EventBase.
 */
class EventBaseStatsCollector : public StatsCollectorBase, public folly::EventBaseObserver {
public:
  static std::shared_ptr<EventBaseStatsCollector>
  create(std::shared_ptr<StatsRegistry> registry, folly::EventBase* evb);

  ~EventBaseStatsCollector() override = default;

  // StatsCollectorBase
  StatsSnapshot snapshot() const override;
  folly::Executor* owningExecutor() const override;

  // folly::EventBaseObserver — sample every loop iteration
  uint32_t getSampleRate() const override { return 1; }
  void loopSample(int64_t busyUs, int64_t idleUs) override;

  // Subscribe to loop samples.  cb is invoked on the EVB thread each iteration.
  // May be called from any thread; the subscription is installed asynchronously.
  // The caller must ensure the objects captured by cb outlive this collector.
  void addLoopObserver(std::function<void(int64_t, int64_t)> cb);

private:
  explicit EventBaseStatsCollector(folly::EventBase* evb);

  folly::EventBase* evb_;

  // Only accessed on evb_ thread.
  std::vector<std::function<void(int64_t, int64_t)>> observers_;

#define DEFINE_HISTOGRAM(name, bounds, unit) BoundedHistogram<bounds.size()> name##_{bounds};
  STATS_EVB_HISTOGRAM_FIELDS(DEFINE_HISTOGRAM)
#undef DEFINE_HISTOGRAM
};

} // namespace openmoq::moqx::stats
