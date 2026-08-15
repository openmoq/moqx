/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <thread>

#include <folly/Chrono.h>
#include <folly/Executor.h>
#include <folly/Function.h>
#include <folly/container/F14Map.h>
#include <folly/logging/xlog.h>
#include <moxygen/MoQTypes.h>

namespace openmoq::moqx::stats {

// Coarse clock: sampled per object on the data path, so it must stay cheap.
// Converted to wall clock at scrape time against a same-instant anchor pair.
using TrackClock = folly::chrono::coarse_steady_clock;

// Which side of the relay a filter counts for.
enum class TrackDirection { Ingest, Egress };

struct DirectionCounters {
  uint64_t groups{0};
  uint64_t subgroups{0};
  uint64_t objects{0};
  uint64_t datagrams{0};
  uint64_t bytes{0};

  DirectionCounters& operator+=(const DirectionCounters& o) {
    groups += o.groups;
    subgroups += o.subgroups;
    objects += o.objects;
    datagrams += o.datagrams;
    bytes += o.bytes;
    return *this;
  }
};

// Per-(track, iothread) counters. Aggregation across threads is operator+=,
// so every field must have a meaningful merge.
struct TrackCounters {
  DirectionCounters received;
  DirectionCounters sent;

  int64_t subscribers{0};

  TrackClock::time_point publishStart{};
  TrackClock::time_point lastObject{};

  DirectionCounters& forDirection(TrackDirection d) {
    return d == TrackDirection::Ingest ? received : sent;
  }

  TrackCounters& operator+=(const TrackCounters& o) {
    received += o.received;
    sent += o.sent;
    subscribers += o.subscribers;

    // Earliest thread to see the track owns the start; latest owns the tail.
    if (o.publishStart != TrackClock::time_point{} &&
        (publishStart == TrackClock::time_point{} || o.publishStart < publishStart)) {
      publishStart = o.publishStart;
    }
    lastObject = std::max(lastObject, o.lastObject);
    return *this;
  }
};

class TrackStatsCollector;

// Owned by the filters counting into it. The collector back-reference is weak
// because collectors die with the relay while a filter chain held by
// SubscriptionRegistry can outlive them.
class TrackStats : public std::enable_shared_from_this<TrackStats> {
public:
  TrackStats(std::weak_ptr<TrackStatsCollector> collector, moxygen::FullTrackName ftn);
  ~TrackStats();

  TrackStats(const TrackStats&) = delete;
  TrackStats& operator=(const TrackStats&) = delete;

  const moxygen::FullTrackName& fullTrackName() const { return ftn_; }

  bool onOwnerThread() const { return std::this_thread::get_id() == owner_; }

  // Consumer chains can be released from any thread, so teardown hops here
  // before touching counters or dropping the last reference. Null once the
  // owning collector is gone.
  folly::Executor* owningExec() const;

  // Filters cache this pointer and never re-consult the collector, so the
  // thread check has to live here rather than only on collector lookups.
  TrackCounters& counters() {
    XDCHECK(onOwnerThread()) << "TrackStats mutated off its owning thread";
    return counters_;
  }

  const TrackCounters& counters() const { return counters_; }

private:
  std::weak_ptr<TrackStatsCollector> collector_;
  moxygen::FullTrackName ftn_;
  TrackCounters counters_;
  std::thread::id owner_{std::this_thread::get_id()};
};

// One instance per (service relay, iothread). All methods must be called on the
// owning thread.
class TrackStatsCollector : public std::enable_shared_from_this<TrackStatsCollector> {
public:
  TrackStatsCollector() = default;

  TrackStatsCollector(const TrackStatsCollector&) = delete;
  TrackStatsCollector& operator=(const TrackStatsCollector&) = delete;

  // Collectors are constructed at init and handed to their thread; this claims
  // ownership from the thread that will actually use them.
  void bindToCurrentThread(folly::Executor* exec) {
    owner_ = std::this_thread::get_id();
    exec_ = exec;
  }

  folly::Executor* exec() const { return exec_; }

  bool onOwnerThread() const { return std::this_thread::get_id() == owner_; }

  // Shares one TrackStats per track per thread, so ingest and every egress
  // filter on this thread count into the same entry.
  std::shared_ptr<TrackStats> getOrCreate(const moxygen::FullTrackName& ftn) {
    checkThread();
    if (auto existing = get(ftn)) {
      return existing;
    }
    auto stats = create(ftn);
    stats->counters().publishStart = TrackClock::now();
    return stats;
  }

  // Creates an entry even when one already exists, displacing it in the slot.
  std::shared_ptr<TrackStats> create(const moxygen::FullTrackName& ftn) {
    checkThread();
    auto stats = std::make_shared<TrackStats>(weak_from_this(), ftn);
    stats_[ftn] = TrackRef{stats.get(), stats};
    return stats;
  }

  std::shared_ptr<TrackStats> get(const moxygen::FullTrackName& ftn) const {
    checkThread();
    auto it = stats_.find(ftn);
    // An entry can outlive its TrackStats when teardown ran off-thread and
    // could not reach us; treat the expired weak ref as absent.
    return it != stats_.end() ? it->second.weak.lock() : nullptr;
  }

  size_t size() const {
    checkThread();
    return stats_.size();
  }

  void forEach(folly::FunctionRef<void(const TrackStats&)> fn) const {
    checkThread();
    for (const auto& [ftn, ref] : stats_) {
      if (auto stats = ref.weak.lock()) {
        fn(*stats);
      }
    }
  }

private:
  friend class TrackStats;

  struct TrackRef {
    // Identity for removal; never dereferenced (weak covers liveness).
    TrackStats* raw{nullptr};
    std::weak_ptr<TrackStats> weak;
  };

  // Identity-checked, so a track that is torn down after a successor claimed
  // the same name cannot evict the successor's entry.
  void remove(const moxygen::FullTrackName& ftn, const TrackStats* expected) {
    checkThread();
    auto it = stats_.find(ftn);
    if (it == stats_.end() || it->second.raw != expected) {
      return;
    }
    stats_.erase(it);
  }

  void checkThread() const {
    XCHECK(onOwnerThread()) << "TrackStatsCollector accessed off its owning thread";
  }

  std::thread::id owner_{std::this_thread::get_id()};
  folly::Executor* exec_{nullptr};
  folly::F14FastMap<moxygen::FullTrackName, TrackRef, moxygen::FullTrackName::hash> stats_;
};

inline folly::Executor* TrackStats::owningExec() const {
  auto collector = collector_.lock();
  return collector ? collector->exec() : nullptr;
}

inline TrackStats::TrackStats(
    std::weak_ptr<TrackStatsCollector> collector,
    moxygen::FullTrackName ftn
)
    : collector_(std::move(collector)), ftn_(std::move(ftn)) {}

// Skips removal when the collector is already gone, or when teardown could not
// reach the owning thread — the weak ref then reports the entry absent.
inline TrackStats::~TrackStats() {
  auto collector = collector_.lock();
  if (collector && collector->onOwnerThread()) {
    collector->remove(ftn_, this);
  }
}

} // namespace openmoq::moqx::stats
