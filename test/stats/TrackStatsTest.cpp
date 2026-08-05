/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "stats/TrackStats.h"

namespace openmoq::moqx::stats {

namespace {

moxygen::FullTrackName makeFtn(std::string ns, std::string name) {
  return moxygen::FullTrackName{moxygen::TrackNamespace(std::move(ns), "/"), std::move(name)};
}

} // namespace

TEST(TrackCountersTest, MergeSumsCounters) {
  TrackCounters a;
  a.received.objects = 3;
  a.received.bytes = 100;
  a.sent.objects = 6;
  a.subscribers = 2;

  TrackCounters b;
  b.received.objects = 4;
  b.received.bytes = 50;
  b.sent.objects = 8;
  b.subscribers = 1;

  a += b;

  EXPECT_EQ(a.received.objects, 7);
  EXPECT_EQ(a.received.bytes, 150);
  EXPECT_EQ(a.sent.objects, 14);
  EXPECT_EQ(a.subscribers, 3);
}

TEST(TrackCountersTest, MergeTakesEarliestStartAndLatestObject) {
  auto t0 = TrackClock::now();
  auto t1 = t0 + std::chrono::seconds(5);

  TrackCounters a;
  a.publishStart = t1;
  a.lastObject = t0;

  TrackCounters b;
  b.publishStart = t0;
  b.lastObject = t1;

  a += b;

  EXPECT_EQ(a.publishStart, t0);
  EXPECT_EQ(a.lastObject, t1);
}

TEST(TrackCountersTest, MergeIgnoresUnsetStart) {
  auto t0 = TrackClock::now();

  TrackCounters a;
  a.publishStart = t0;

  a += TrackCounters{};

  EXPECT_EQ(a.publishStart, t0);
}

TEST(TrackStatsCollectorTest, GetOrCreateSharesOneEntryPerTrack) {
  auto collectorPtr = std::make_shared<TrackStatsCollector>();
  auto& collector = *collectorPtr;
  auto ftn = makeFtn("ns", "track");

  auto first = collector.getOrCreate(ftn);
  auto second = collector.getOrCreate(ftn);

  EXPECT_EQ(first.get(), second.get());
  EXPECT_EQ(collector.size(), 1);
  EXPECT_NE(first->counters().publishStart, TrackClock::time_point{});
}

TEST(TrackStatsCollectorTest, EntryRemovedWhenLastRefDropped) {
  auto collectorPtr = std::make_shared<TrackStatsCollector>();
  auto& collector = *collectorPtr;
  auto ftn = makeFtn("ns", "track");

  {
    auto stats = collector.getOrCreate(ftn);
    EXPECT_EQ(collector.size(), 1);
    EXPECT_NE(collector.get(ftn), nullptr);
  }

  EXPECT_EQ(collector.size(), 0);
  EXPECT_EQ(collector.get(ftn), nullptr);
}

TEST(TrackStatsCollectorTest, TeardownDoesNotEvictSuccessorWithSameName) {
  auto collectorPtr = std::make_shared<TrackStatsCollector>();
  auto& collector = *collectorPtr;
  auto ftn = makeFtn("ns", "track");

  auto original = collector.getOrCreate(ftn);
  // Force a successor into the slot, as a reconnecting publisher would.
  auto successor = collector.create(ftn);
  ASSERT_NE(original.get(), successor.get());
  ASSERT_EQ(collector.get(ftn).get(), successor.get());

  original.reset();

  EXPECT_EQ(collector.size(), 1);
  EXPECT_EQ(collector.get(ftn).get(), successor.get());
}

TEST(TrackStatsCollectorTest, ForEachVisitsLiveTracks) {
  auto collectorPtr = std::make_shared<TrackStatsCollector>();
  auto& collector = *collectorPtr;
  auto a = collector.getOrCreate(makeFtn("ns", "a"));
  auto b = collector.getOrCreate(makeFtn("ns", "b"));
  a->counters().received.objects = 2;
  b->counters().received.objects = 3;

  uint64_t total = 0;
  collector.forEach([&](const TrackStats& stats) { total += stats.counters().received.objects; });

  EXPECT_EQ(total, 5);
}

} // namespace openmoq::moqx::stats
