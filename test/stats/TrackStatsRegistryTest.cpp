/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/coro/BlockingWait.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <gtest/gtest.h>

#include "stats/TrackStatsRegistry.h"

namespace openmoq::moqx::stats {

namespace {

moxygen::FullTrackName makeFtn(std::string ns, std::string name) {
  return moxygen::FullTrackName{moxygen::TrackNamespace(std::move(ns), "/"), std::move(name)};
}

} // namespace

class TrackStatsRegistryTest : public ::testing::Test {
protected:
  void SetUp() override { registry_.bindAll({threadA_.getEventBase(), threadB_.getEventBase()}); }

  // Counts on the thread that owns the collector, as the data-plane filters do,
  // and holds the entry alive the way a filter in a chain would.
  void countOn(folly::EventBase* evb, const moxygen::FullTrackName& ftn, uint64_t objects) {
    evb->runInEventBaseThreadAndWait([&] {
      auto* collector = registry_.currentCollector();
      ASSERT_NE(collector, nullptr);
      auto stats = collector->getOrCreate(ftn);
      stats->counters().objectsReceived += objects;
      stats->counters().subscribers += 1;
      stats->counters().lastObject = TrackClock::now();
      held_.push_back(std::move(stats));
    });
  }

  TrackCounters countersOn(folly::EventBase* evb, const moxygen::FullTrackName& ftn) {
    TrackCounters counters;
    evb->runInEventBaseThreadAndWait([&] {
      if (auto stats = registry_.currentCollector()->get(ftn)) {
        counters = stats->counters();
      }
    });
    return counters;
  }

  folly::ScopedEventBaseThread threadA_{"track-stats-a"};
  folly::ScopedEventBaseThread threadB_{"track-stats-b"};
  TrackStatsRegistry registry_;
  std::vector<std::shared_ptr<TrackStats>> held_;
};

TEST_F(TrackStatsRegistryTest, MergesCountersAcrossThreads) {
  auto ftn = makeFtn("ns", "track");
  countOn(threadA_.getEventBase(), ftn, 3);
  countOn(threadB_.getEventBase(), ftn, 4);

  auto merged = folly::coro::blockingWait(registry_.aggregateAsync({ftn}));

  ASSERT_EQ(merged.size(), 1);
  EXPECT_EQ(merged[ftn].objectsReceived, 7);
  EXPECT_EQ(merged[ftn].subscribers, 2);
}

TEST_F(TrackStatsRegistryTest, ReportsOnlyRequestedKeys) {
  auto wanted = makeFtn("ns", "wanted");
  auto other = makeFtn("ns", "other");
  countOn(threadA_.getEventBase(), wanted, 1);
  countOn(threadA_.getEventBase(), other, 5);

  auto merged = folly::coro::blockingWait(registry_.aggregateAsync({wanted}));

  EXPECT_EQ(merged.size(), 1);
  EXPECT_EQ(merged[wanted].objectsReceived, 1);
}

TEST_F(TrackStatsRegistryTest, UnknownKeyIsAbsentRatherThanZero) {
  auto merged = folly::coro::blockingWait(registry_.aggregateAsync({makeFtn("ns", "never-published")}));
  EXPECT_TRUE(merged.empty());
}

TEST_F(TrackStatsRegistryTest, MergeTakesEarliestStartAndLatestObject) {
  auto ftn = makeFtn("ns", "track");
  countOn(threadA_.getEventBase(), ftn, 1);
  countOn(threadB_.getEventBase(), ftn, 1);

  auto a = countersOn(threadA_.getEventBase(), ftn);
  auto b = countersOn(threadB_.getEventBase(), ftn);
  auto merged = folly::coro::blockingWait(registry_.aggregateAsync({ftn}));

  EXPECT_EQ(merged[ftn].publishStart, std::min(a.publishStart, b.publishStart));
  EXPECT_EQ(merged[ftn].lastObject, std::max(a.lastObject, b.lastObject));
}

TEST_F(TrackStatsRegistryTest, EntryDisappearsWhenLastRefDropped) {
  auto ftn = makeFtn("ns", "track");
  countOn(threadA_.getEventBase(), ftn, 1);
  ASSERT_EQ(folly::coro::blockingWait(registry_.aggregateAsync({ftn})).size(), 1);

  threadA_.getEventBase()->runInEventBaseThreadAndWait([&] { held_.clear(); });

  EXPECT_TRUE(folly::coro::blockingWait(registry_.aggregateAsync({ftn})).empty());
}

TEST_F(TrackStatsRegistryTest, UnboundRegistryCountsNothing) {
  TrackStatsRegistry unbound;
  EXPECT_EQ(unbound.currentCollector(), nullptr);
  EXPECT_TRUE(folly::coro::blockingWait(unbound.aggregateAsync({makeFtn("ns", "track")})).empty());
}

} // namespace openmoq::moqx::stats
