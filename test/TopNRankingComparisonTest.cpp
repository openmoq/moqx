/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * Comparison tests between PropertyRanking (Complex) and SimpleTopNRanking (Simple).
 *
 * Verifies behavioral equivalence for viewer scenarios (non-publisher-subscriber).
 * Note: Self-exclusion behavior differs by design:
 * - Complex: Handles self-exclusion with waterline tracking
 * - Simple: Relies on moq-transport layer for self-exclusion
 */

#include "relay/TopNRankingFactory.h"

#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <moxygen/test/MockMoQSession.h>

using namespace testing;
using namespace moxygen;
using namespace openmoq::moqx;

namespace {

const TrackNamespace kNs{{"test"}};
constexpr uint64_t kProp = 0x100;

FullTrackName ftn(const std::string& name) {
  return FullTrackName{kNs, name};
}

struct EventRecorder {
  struct SelectEvent {
    FullTrackName ftn;
    MoQSession* session;
    bool forward;
  };
  struct EvictEvent {
    FullTrackName ftn;
    MoQSession* session;
  };

  std::vector<SelectEvent> selected;
  std::vector<EvictEvent> evicted;

  void clear() {
    selected.clear();
    evicted.clear();
  }

  int selectCount(const FullTrackName& f, MoQSession* s) const {
    int n = 0;
    for (const auto& e : selected) {
      if (e.ftn == f && e.session == s)
        n++;
    }
    return n;
  }

  int evictCount(const FullTrackName& f, MoQSession* s) const {
    int n = 0;
    for (const auto& e : evicted) {
      if (e.ftn == f && e.session == s)
        n++;
    }
    return n;
  }

  bool wasSelected(const FullTrackName& f, MoQSession* s) const { return selectCount(f, s) > 0; }

  bool wasEvicted(const FullTrackName& f, MoQSession* s) const { return evictCount(f, s) > 0; }
};

class TopNRankingComparisonTest : public ::testing::TestWithParam<TopNRankingMode> {
protected:
  std::shared_ptr<MoQSession> makeSession() {
    auto s = std::make_shared<NiceMock<moxygen::test::MockMoQSession>>();
    sessions_.push_back(s);
    return std::static_pointer_cast<MoQSession>(s);
  }

  std::shared_ptr<ITopNRanking> makeRanking(EventRecorder& recorder) {
    return TopNRankingFactory::create(
        GetParam(),
        kProp,
        /*maxDeselected=*/5,
        /*idleTimeout=*/std::chrono::milliseconds{0},
        /*sweepThrottle=*/std::chrono::milliseconds{0},
        /*getLastActivity=*/
        [this](const FullTrackName& f) {
          auto it = activityTimes_.find(f);
          if (it == activityTimes_.end()) {
            return std::chrono::steady_clock::time_point{};
          }
          return it->second;
        },
        /*onBatchSelected=*/
        [&recorder](
            const FullTrackName& f,
            const std::vector<std::pair<std::shared_ptr<MoQSession>, bool>>& sessions
        ) {
          for (auto& [s, fwd] : sessions) {
            recorder.selected.push_back({f, s.get(), fwd});
          }
        },
        /*onSelected=*/
        [&recorder](const FullTrackName& f, std::shared_ptr<MoQSession> s, bool fwd) {
          recorder.selected.push_back({f, s.get(), fwd});
        },
        /*onEvicted=*/
        [&recorder](const FullTrackName& f, std::shared_ptr<MoQSession> s) {
          recorder.evicted.push_back({f, s.get()});
        }
    );
  }

  void setActivityTime(const FullTrackName& f, std::chrono::steady_clock::time_point t) {
    activityTimes_[f] = t;
  }

  std::vector<std::shared_ptr<NiceMock<moxygen::test::MockMoQSession>>> sessions_;
  std::shared_ptr<MoQSession> defaultPublisher_{makeSession()};
  folly::F14FastMap<FullTrackName, std::chrono::steady_clock::time_point, FullTrackName::hash>
      activityTimes_;
};

// ---------------------------------------------------------------------------
// Basic viewer scenarios - should behave identically
// ---------------------------------------------------------------------------

TEST_P(TopNRankingComparisonTest, RegisterThenSubscribe_TopNBoundary) {
  EventRecorder recorder;
  auto ranking = makeRanking(recorder);

  ranking->registerTrack(ftn("a"), 90, defaultPublisher_);
  ranking->registerTrack(ftn("b"), 80, defaultPublisher_);
  ranking->registerTrack(ftn("c"), 70, defaultPublisher_);
  ranking->registerTrack(ftn("d"), 20, defaultPublisher_);

  auto sub = makeSession();
  ranking->addSessionToTopNGroup(2, sub, true);

  EXPECT_TRUE(recorder.wasSelected(ftn("a"), sub.get()));
  EXPECT_TRUE(recorder.wasSelected(ftn("b"), sub.get()));
  EXPECT_FALSE(recorder.wasSelected(ftn("c"), sub.get()));
  EXPECT_FALSE(recorder.wasSelected(ftn("d"), sub.get()));
}

TEST_P(TopNRankingComparisonTest, SubscribeThenRegister_TopNBoundary) {
  EventRecorder recorder;
  auto ranking = makeRanking(recorder);

  auto sub = makeSession();
  ranking->addSessionToTopNGroup(2, sub, true);

  ranking->registerTrack(ftn("a"), 90, defaultPublisher_);
  ranking->registerTrack(ftn("b"), 80, defaultPublisher_);
  ranking->registerTrack(ftn("c"), 70, defaultPublisher_);

  EXPECT_TRUE(recorder.wasSelected(ftn("a"), sub.get()));
  EXPECT_TRUE(recorder.wasSelected(ftn("b"), sub.get()));
  EXPECT_FALSE(recorder.wasSelected(ftn("c"), sub.get()));
}

TEST_P(TopNRankingComparisonTest, UpdateSortValue_CrossesThreshold) {
  EventRecorder recorder;
  auto ranking = makeRanking(recorder);

  auto sub = makeSession();
  ranking->addSessionToTopNGroup(1, sub, true);

  ranking->registerTrack(ftn("a"), 80, defaultPublisher_);
  ranking->registerTrack(ftn("b"), 60, defaultPublisher_);
  recorder.clear();

  // "b" jumps to rank 0
  ranking->updateSortValue(ftn("b"), 90);

  EXPECT_TRUE(recorder.wasSelected(ftn("b"), sub.get()));
}

TEST_P(TopNRankingComparisonTest, RemoveSelected_PromotesNext) {
  EventRecorder recorder;
  auto ranking = makeRanking(recorder);

  auto sub = makeSession();
  ranking->addSessionToTopNGroup(1, sub, true);

  ranking->registerTrack(ftn("a"), 90, defaultPublisher_);
  ranking->registerTrack(ftn("b"), 70, defaultPublisher_);
  recorder.clear();

  ranking->removeTrack(ftn("a"));

  EXPECT_TRUE(recorder.wasEvicted(ftn("a"), sub.get()));
  EXPECT_TRUE(recorder.wasSelected(ftn("b"), sub.get()));
}

TEST_P(TopNRankingComparisonTest, MultipleSubscribersDifferentN) {
  EventRecorder recorder;
  auto ranking = makeRanking(recorder);

  auto sub1 = makeSession();
  auto sub3 = makeSession();

  ranking->addSessionToTopNGroup(1, sub1, true);
  ranking->addSessionToTopNGroup(3, sub3, true);

  ranking->registerTrack(ftn("a"), 100, defaultPublisher_);
  ranking->registerTrack(ftn("b"), 80, defaultPublisher_);
  ranking->registerTrack(ftn("c"), 60, defaultPublisher_);
  ranking->registerTrack(ftn("d"), 40, defaultPublisher_);

  // sub1 (top-1) should have received only "a"
  EXPECT_EQ(recorder.selectCount(ftn("a"), sub1.get()), 1);
  EXPECT_EQ(recorder.selectCount(ftn("b"), sub1.get()), 0);

  // sub3 (top-3) should have received "a", "b", "c"
  EXPECT_EQ(recorder.selectCount(ftn("a"), sub3.get()), 1);
  EXPECT_EQ(recorder.selectCount(ftn("b"), sub3.get()), 1);
  EXPECT_EQ(recorder.selectCount(ftn("c"), sub3.get()), 1);
  EXPECT_EQ(recorder.selectCount(ftn("d"), sub3.get()), 0);
}

TEST_P(TopNRankingComparisonTest, ForwardFlagPassedThrough) {
  EventRecorder recorder;
  auto ranking = makeRanking(recorder);

  auto sub = makeSession();
  ranking->addSessionToTopNGroup(1, sub, false); // forward=false
  ranking->registerTrack(ftn("a"), 10, defaultPublisher_);

  ASSERT_EQ(recorder.selected.size(), 1u);
  EXPECT_FALSE(recorder.selected[0].forward);
}

TEST_P(TopNRankingComparisonTest, SessionRemoval) {
  EventRecorder recorder;
  auto ranking = makeRanking(recorder);

  auto sub1 = makeSession();
  auto sub2 = makeSession();

  ranking->addSessionToTopNGroup(2, sub1, true);
  ranking->addSessionToTopNGroup(2, sub2, true);

  ranking->removeSessionFromTopNGroup(2, sub1);

  EXPECT_FALSE(ranking->empty());

  ranking->removeSessionFromTopNGroup(2, sub2);

  EXPECT_TRUE(ranking->empty());
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

TEST_P(TopNRankingComparisonTest, DuplicateRegisterIgnored) {
  EventRecorder recorder;
  auto ranking = makeRanking(recorder);

  ranking->registerTrack(ftn("a"), 100, defaultPublisher_);
  ranking->registerTrack(ftn("a"), 200, defaultPublisher_); // duplicate

  EXPECT_EQ(ranking->numTracks(), 1u);
}

TEST_P(TopNRankingComparisonTest, UpdateUnknownTrack_NoOp) {
  EventRecorder recorder;
  auto ranking = makeRanking(recorder);

  ranking->updateSortValue(ftn("unknown"), 100);

  EXPECT_EQ(recorder.selected.size(), 0u);
}

TEST_P(TopNRankingComparisonTest, RemoveUnknownTrack_NoOp) {
  EventRecorder recorder;
  auto ranking = makeRanking(recorder);

  ranking->removeTrack(ftn("unknown"));

  EXPECT_EQ(recorder.evicted.size(), 0u);
}

// Instantiate tests for both implementations
INSTANTIATE_TEST_SUITE_P(
    BothImplementations,
    TopNRankingComparisonTest,
    ::testing::Values(TopNRankingMode::Complex, TopNRankingMode::Simple),
    [](const ::testing::TestParamInfo<TopNRankingMode>& info) {
      return info.param == TopNRankingMode::Complex ? "Complex" : "Simple";
    }
);

// ---------------------------------------------------------------------------
// Tests specific to behavioral differences
// ---------------------------------------------------------------------------

class TopNRankingDifferenceTest : public ::testing::Test {
protected:
  std::shared_ptr<MoQSession> makeSession() {
    auto s = std::make_shared<NiceMock<moxygen::test::MockMoQSession>>();
    sessions_.push_back(s);
    return std::static_pointer_cast<MoQSession>(s);
  }

  std::vector<std::shared_ptr<NiceMock<moxygen::test::MockMoQSession>>> sessions_;
  std::shared_ptr<MoQSession> defaultPublisher_{makeSession()};
};

TEST_F(TopNRankingDifferenceTest, SelfExclusion_BothImplementationsExclude) {
  // Both implementations now handle self-exclusion at the ranking layer:
  // - Complex: Self-exclusion at tracker level (waterline-based)
  // - Simple: Self-exclusion at SimpleTopNRanking level (computeTopNForSession)
  //
  // Both exclude self-tracks from callbacks, achieving identical behavior.

  EventRecorder complexRecorder, simpleRecorder;

  // Create both implementations
  auto complexRanking = TopNRankingFactory::create(
      TopNRankingMode::Complex,
      kProp,
      5,
      std::chrono::milliseconds{0},
      std::chrono::milliseconds{0},
      [](const FullTrackName&) { return std::chrono::steady_clock::time_point{}; },
      [&complexRecorder](
          const FullTrackName& f,
          const std::vector<std::pair<std::shared_ptr<MoQSession>, bool>>& sessions
      ) {
        for (auto& [s, fwd] : sessions) {
          complexRecorder.selected.push_back({f, s.get(), fwd});
        }
      },
      [&complexRecorder](const FullTrackName& f, std::shared_ptr<MoQSession> s, bool fwd) {
        complexRecorder.selected.push_back({f, s.get(), fwd});
      },
      [&complexRecorder](const FullTrackName& f, std::shared_ptr<MoQSession> s) {
        complexRecorder.evicted.push_back({f, s.get()});
      }
  );

  auto simpleRanking = TopNRankingFactory::create(
      TopNRankingMode::Simple,
      kProp,
      5,
      std::chrono::milliseconds{0},
      std::chrono::milliseconds{0},
      [](const FullTrackName&) { return std::chrono::steady_clock::time_point{}; },
      [&simpleRecorder](
          const FullTrackName& f,
          const std::vector<std::pair<std::shared_ptr<MoQSession>, bool>>& sessions
      ) {
        for (auto& [s, fwd] : sessions) {
          simpleRecorder.selected.push_back({f, s.get(), fwd});
        }
      },
      [&simpleRecorder](const FullTrackName& f, std::shared_ptr<MoQSession> s, bool fwd) {
        simpleRecorder.selected.push_back({f, s.get(), fwd});
      },
      [&simpleRecorder](const FullTrackName& f, std::shared_ptr<MoQSession> s) {
        simpleRecorder.evicted.push_back({f, s.get()});
      }
  );

  auto pubSub = makeSession(); // Publisher-subscriber

  // Register track with pubSub as publisher
  complexRanking->registerTrack(ftn("self"), 100, pubSub);
  simpleRanking->registerTrack(ftn("self"), 100, pubSub);

  // pubSub subscribes (wants top-1)
  complexRanking->addSessionToTopNGroup(1, pubSub, true);
  simpleRanking->addSessionToTopNGroup(1, pubSub, true);

  // Both implementations exclude self-tracks from selection callbacks
  EXPECT_FALSE(complexRecorder.wasSelected(ftn("self"), pubSub.get()))
      << "Complex excludes self-track at tracker level via waterline";

  EXPECT_FALSE(simpleRecorder.wasSelected(ftn("self"), pubSub.get()))
      << "Simple excludes self-track at ranking level via computeTopNForSession";
}

TEST_F(TopNRankingDifferenceTest, SelfExclusion_CorrectNCount) {
  // Key test: If subscriber wants top-3 and their own track is at rank #2,
  // they should receive tracks at ranks #1, #3, #4 (skipping self).
  // This ensures they get exactly N=3 tracks, not N-1=2.

  EventRecorder simpleRecorder;

  auto simpleRanking = TopNRankingFactory::create(
      TopNRankingMode::Simple,
      kProp,
      5,
      std::chrono::milliseconds{0},
      std::chrono::milliseconds{0},
      [](const FullTrackName&) { return std::chrono::steady_clock::time_point{}; },
      [&simpleRecorder](
          const FullTrackName& f,
          const std::vector<std::pair<std::shared_ptr<MoQSession>, bool>>& sessions
      ) {
        for (auto& [s, fwd] : sessions) {
          simpleRecorder.selected.push_back({f, s.get(), fwd});
        }
      },
      [&simpleRecorder](const FullTrackName& f, std::shared_ptr<MoQSession> s, bool fwd) {
        simpleRecorder.selected.push_back({f, s.get(), fwd});
      },
      [&simpleRecorder](const FullTrackName& f, std::shared_ptr<MoQSession> s) {
        simpleRecorder.evicted.push_back({f, s.get()});
      }
  );

  auto pubSub = makeSession();
  auto otherPub = makeSession();

  // Register tracks: pubSub's track is at rank #2 (val=85)
  simpleRanking->registerTrack(ftn("a"), 100, otherPub); // rank 0
  simpleRanking->registerTrack(ftn("b"), 90, otherPub);  // rank 1
  simpleRanking->registerTrack(ftn("self"), 85, pubSub); // rank 2 (pubSub's)
  simpleRanking->registerTrack(ftn("c"), 80, otherPub);  // rank 3
  simpleRanking->registerTrack(ftn("d"), 70, otherPub);  // rank 4
  simpleRanking->registerTrack(ftn("e"), 60, otherPub);  // rank 5

  // pubSub subscribes wanting top-3
  simpleRanking->addSessionToTopNGroup(3, pubSub, true);

  // pubSub should receive exactly 3 tracks: a, b, c (skipping "self")
  // NOT just a, b (which would be wrong - only 2 tracks)
  int count = 0;
  for (const auto& e : simpleRecorder.selected) {
    if (e.session == pubSub.get()) {
      count++;
    }
  }

  EXPECT_EQ(count, 3) << "pubSub should receive exactly 3 tracks";
  EXPECT_TRUE(simpleRecorder.wasSelected(ftn("a"), pubSub.get()));
  EXPECT_TRUE(simpleRecorder.wasSelected(ftn("b"), pubSub.get()));
  EXPECT_FALSE(simpleRecorder.wasSelected(ftn("self"), pubSub.get()))
      << "Self-track should be excluded";
  EXPECT_TRUE(simpleRecorder.wasSelected(ftn("c"), pubSub.get()))
      << "Should promote c to fill the N=3 slot";
  EXPECT_FALSE(simpleRecorder.wasSelected(ftn("d"), pubSub.get()))
      << "d is beyond N=3 with self-exclusion";
}

TEST_F(TopNRankingDifferenceTest, CodeComplexityComparison) {
  // This test documents the complexity difference - not a functional test

  // PropertyRanking (Complex):
  // - ~728 lines of implementation
  // - Multiple data structures: std::map, F14FastMap per TopNGroup, deselectedQueue
  // - Per-session state: waterlineKey, selectedTracks set
  // - Self-exclusion: publisherTrackCount_, reconcilePublisherSelection, computeWaterlineKey
  // - Threshold management: selectionThreshold_, publisherExtendedThreshold_, sortedThresholds_

  // SimpleTopNTracker (Simple):
  // - ~300 lines of implementation
  // - Single atomic snapshot (shared_ptr<vector>)
  // - Per-session state: just N value and forward flag
  // - Self-exclusion: handled at transport layer (per moq-transport spec)
  // - Threshold management: single atomic max_n

  // This is a documentation-only test - always passes
  SUCCEED() << "Simple design is ~60% less code than Complex design";
}

} // namespace
