/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "stats/TrackStatsRegistry.h"

#include <algorithm>

#include <folly/coro/Collect.h>
#include <folly/futures/Future.h>
#include <folly/logging/xlog.h>

namespace openmoq::moqx::stats {

void TrackStatsRegistry::bindAll(const std::vector<folly::Executor*>& execs) {
  for (auto* exec : execs) {
    XCHECK(exec) << "TrackStatsRegistry::bindAll: null executor";
    // Blocks so every thread is bound before serving, and so a queued bind can
    // never outlive this registry; mirrors
    // MoqxRelayContext::initThreadStatsCollectors.
    folly::via(exec, [this, exec] { bind(exec); }).get();
  }
}

void TrackStatsRegistry::bind(folly::Executor* exec) {
  XCHECK(!*tlCollector_) << "track stats already bound on this thread";
  auto collector = std::make_shared<TrackStatsCollector>();
  collector->bindToCurrentThread(exec);
  collectors_.push_back({exec, collector});
  *tlCollector_ = std::move(collector);
}

folly::coro::Task<TrackCountersMap>
TrackStatsRegistry::aggregateAsync(std::vector<moxygen::FullTrackName> keys) const {
  auto snapshotOne = [](std::shared_ptr<TrackStatsCollector> collector,
                        const std::vector<moxygen::FullTrackName>& keys
                     ) -> folly::coro::Task<TrackCountersMap> {
    TrackCountersMap partial;
    for (const auto& ftn : keys) {
      if (auto stats = collector->get(ftn)) {
        partial[ftn] = stats->counters();
      }
    }
    co_return partial;
  };

  std::vector<folly::coro::TaskWithExecutor<TrackCountersMap>> tasks;
  tasks.reserve(collectors_.size());
  for (const auto& bound : collectors_) {
    tasks.push_back(folly::coro::co_withExecutor(bound.exec, snapshotOne(bound.collector, keys)));
  }

  auto results = co_await folly::coro::collectAllRange(std::move(tasks));
  TrackCountersMap combined;
  for (const auto& partial : results) {
    for (const auto& [ftn, counters] : partial) {
      combined[ftn] += counters;
    }
  }
  co_return combined;
}

} // namespace openmoq::moqx::stats
