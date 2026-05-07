/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "stats/EventBaseStatsCollector.h"

namespace openmoq::moqx::stats {

/* static */
std::shared_ptr<EventBaseStatsCollector>
EventBaseStatsCollector::create(std::shared_ptr<StatsRegistry> registry, folly::EventBase* evb) {
  auto collector = std::shared_ptr<EventBaseStatsCollector>(new EventBaseStatsCollector(evb));
  registry->registerEvbCollector(evb, collector);
  evb->runImmediatelyOrRunInEventBaseThread([evb, collector] { evb->setObserver(collector); });
  return collector;
}

EventBaseStatsCollector::EventBaseStatsCollector(folly::EventBase* evb) : evb_(evb) {}

void EventBaseStatsCollector::addLoopObserver(std::function<void(int64_t, int64_t)> cb) {
  evb_->runImmediatelyOrRunInEventBaseThread([this, cb = std::move(cb)]() mutable {
    observers_.emplace_back(std::move(cb));
  });
}

void EventBaseStatsCollector::loopSample(int64_t busyUs, int64_t idleUs) {
  evbLoopBusy_.addValue(static_cast<uint64_t>(busyUs));
  evbLoopIdle_.addValue(static_cast<uint64_t>(idleUs));

  for (auto& cb : observers_) {
    cb(busyUs, idleUs);
  }
}

StatsSnapshot EventBaseStatsCollector::snapshot() const {
  StatsSnapshot snap;

#define COPY_HISTOGRAM(name, bounds, unit)                                                         \
  name##_.fillCumulative(snap.name##Buckets);                                                      \
  snap.name##Sum = name##_.sum;                                                                    \
  snap.name##Count = name##_.count;
  STATS_EVB_HISTOGRAM_FIELDS(COPY_HISTOGRAM)
#undef COPY_HISTOGRAM

  return snap;
}

folly::Executor* EventBaseStatsCollector::owningExecutor() const {
  return evb_;
}

} // namespace openmoq::moqx::stats
