/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <vector>

#include <folly/Executor.h>
#include <folly/ThreadLocal.h>
#include <folly/container/F14Map.h>
#include <folly/coro/Task.h>

#include "stats/TrackStats.h"

namespace openmoq::moqx::stats {

using TrackCountersMap =
    folly::F14FastMap<moxygen::FullTrackName, TrackCounters, moxygen::FullTrackName::hash>;

// Owns one TrackStatsCollector per data-plane thread and merges them on demand.
//
// Collectors are created before serving and never added afterwards, so the
// thread list is read lock-free — the contract StatsRegistry::collectors_ uses.
class TrackStatsRegistry {
public:
  // Binds a collector on each executor's thread, blocking until every one is
  // bound. Must run before any listener accepts sessions, and not on a thread
  // that drives one of execs — the wait would deadlock.
  void bindAll(const std::vector<folly::Executor*>& execs);

  // Null on threads that were never bound; callers skip counting rather than
  // fail, so tests and benchmarks can leave collectors unbound.
  TrackStatsCollector* currentCollector() { return tlCollector_->get(); }

  folly::coro::Task<TrackCountersMap> aggregateAsync(std::vector<moxygen::FullTrackName> keys
  ) const;

private:
  struct BoundCollector {
    folly::Executor* exec;
    std::shared_ptr<TrackStatsCollector> collector;
  };

  void bind(folly::Executor* exec);

  folly::ThreadLocal<std::shared_ptr<TrackStatsCollector>> tlCollector_;
  std::vector<BoundCollector> collectors_;
};

} // namespace openmoq::moqx::stats
