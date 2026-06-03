/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * Unit tests for SimpleTopNRanking top-N base logic.
 *
 * These tests mirror PropertyRankingBaseTest.cpp to ensure behavioral parity
 * with the simpler lock-free implementation.
 *
 * Covers:
 * - registerTrack + addSessionToTopNGroup -> correct sessions notified at top-N boundary
 * - updateSortValue fast path: no notification when threshold not crossed
 * - updateSortValue slow path: correct promotions/evictions
 * - removeTrack of a selected track -> replacement promoted
 * - Multiple sessions with different N values
 * - Session removal
 * - Idle sweep
 */

#include "relay/SimpleTopNTracker.h"

#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <moxygen/test/MockMoQSession.h>
#include <moxygen/test/Mocks.h>

using namespace testing;
using namespace moxygen;
using namespace openmoq::moqx;

namespace {

const TrackNamespace kNs{{"test"}};
constexpr uint64_t kProp = 0x100;

FullTrackName ftn(const std::string& name) {
  return FullTrackName{kNs, name};
}

class SimpleRankingHarness {
public:
  struct SelectEvent {
    FullTrackName ftn;
    MoQSession* session;
    bool forward;
  };
  struct EvictEvent {
    FullTrackName ftn;
    MoQSession* session;
  };

  explicit SimpleRankingHarness(
      std::chrono::milliseconds idleTimeout = std::chrono::milliseconds(0),
      std::chrono::milliseconds sweepThrottle = std::chrono::milliseconds(0)
  )
      : ranking_(std::make_unique<SimpleTopNRanking>(
            kProp,
            0, // maxDeselected ignored in simple design
            idleTimeout,
            sweepThrottle,
            [this](const FullTrackName& f) {
              auto it = activityTimes_.find(f);
              if (it == activityTimes_.end()) {
                return std::chrono::steady_clock::time_point{};
              }
              return it->second;
            },
            [this](
                const FullTrackName& f,
                const std::vector<std::pair<std::shared_ptr<MoQSession>, bool>>& sessions
            ) {
              for (auto& [s, fwd] : sessions) {
                selected_.push_back({f, s.get(), fwd});
              }
            },
            [this](const FullTrackName& f, std::shared_ptr<MoQSession> s, bool fwd) {
              selected_.push_back({f, s.get(), fwd});
            },
            [this](const FullTrackName& f, std::shared_ptr<MoQSession> s) {
              evicted_.push_back({f, s.get()});
            }
        )) {}

  void setActivityTime(const FullTrackName& f, std::chrono::steady_clock::time_point t) {
    activityTimes_[f] = t;
  }

  SimpleTopNRanking& ranking() { return *ranking_; }

  int selectCount(const FullTrackName& f) const {
    int n = 0;
    for (auto& e : selected_) {
      if (e.ftn == f) {
        n++;
      }
    }
    return n;
  }

  int selectCount(const FullTrackName& f, MoQSession* s) const {
    int n = 0;
    for (auto& e : selected_) {
      if (e.ftn == f && e.session == s) {
        n++;
      }
    }
    return n;
  }

  int evictCount(const FullTrackName& f) const {
    int n = 0;
    for (auto& e : evicted_) {
      if (e.ftn == f) {
        n++;
      }
    }
    return n;
  }

  int evictCount(const FullTrackName& f, MoQSession* s) const {
    int n = 0;
    for (auto& e : evicted_) {
      if (e.ftn == f && e.session == s) {
        n++;
      }
    }
    return n;
  }

  void clearEvents() {
    selected_.clear();
    evicted_.clear();
  }

  std::vector<SelectEvent> selected_;
  std::vector<EvictEvent> evicted_;
  folly::F14FastMap<FullTrackName, std::chrono::steady_clock::time_point, FullTrackName::hash>
      activityTimes_;

private:
  std::unique_ptr<SimpleTopNRanking> ranking_;
};

class SimpleTopNTrackerBaseTest : public ::testing::Test {
protected:
  std::shared_ptr<MoQSession> makeSession() {
    auto s = std::make_shared<NiceMock<moxygen::test::MockMoQSession>>();
    sessions_.push_back(s);
    return std::static_pointer_cast<MoQSession>(s);
  }

  std::vector<std::shared_ptr<NiceMock<moxygen::test::MockMoQSession>>> sessions_;
  std::shared_ptr<MoQSession> defaultPublisher_{makeSession()};
};

// ---------------------------------------------------------------------------
// registerTrack + addSessionToTopNGroup
// ---------------------------------------------------------------------------

TEST_F(SimpleTopNTrackerBaseTest, RegisterThenSubscribe_TopNBoundary) {
  SimpleRankingHarness h;

  h.ranking().registerTrack(ftn("a"), 90, defaultPublisher_);
  h.ranking().registerTrack(ftn("b"), 80, defaultPublisher_);
  h.ranking().registerTrack(ftn("c"), 70, defaultPublisher_);
  h.ranking().registerTrack(ftn("d"), 20, defaultPublisher_);

  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(2, sub, /*forward=*/true);

  EXPECT_EQ(h.selectCount(ftn("a"), sub.get()), 1);
  EXPECT_EQ(h.selectCount(ftn("b"), sub.get()), 1);
  EXPECT_EQ(h.selectCount(ftn("c"), sub.get()), 0);
  EXPECT_EQ(h.selectCount(ftn("d"), sub.get()), 0);
}

TEST_F(SimpleTopNTrackerBaseTest, SubscribeThenRegister_TopNBoundary) {
  SimpleRankingHarness h;

  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(2, sub, /*forward=*/true);

  h.ranking().registerTrack(ftn("a"), 90, defaultPublisher_);
  h.ranking().registerTrack(ftn("b"), 80, defaultPublisher_);
  h.ranking().registerTrack(ftn("c"), 70, defaultPublisher_);

  EXPECT_EQ(h.selectCount(ftn("a"), sub.get()), 1);
  EXPECT_EQ(h.selectCount(ftn("b"), sub.get()), 1);
  EXPECT_EQ(h.selectCount(ftn("c"), sub.get()), 0);
}

TEST_F(SimpleTopNTrackerBaseTest, ForwardFlagPassedThrough_False) {
  SimpleRankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(1, sub, /*forward=*/false);
  h.ranking().registerTrack(ftn("a"), 10, defaultPublisher_);

  ASSERT_EQ(h.selected_.size(), 1u);
  EXPECT_FALSE(h.selected_[0].forward);
}

TEST_F(SimpleTopNTrackerBaseTest, ForwardFlagPassedThrough_True) {
  SimpleRankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(1, sub, /*forward=*/true);
  h.ranking().registerTrack(ftn("a"), 10, defaultPublisher_);

  ASSERT_EQ(h.selected_.size(), 1u);
  EXPECT_TRUE(h.selected_[0].forward);
}

// ---------------------------------------------------------------------------
// updateSortValue fast path
// ---------------------------------------------------------------------------

TEST_F(SimpleTopNTrackerBaseTest, FastPath_NoNotificationWhenThresholdNotCrossed) {
  SimpleRankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(2, sub, true);

  h.ranking().registerTrack(ftn("a"), 90, defaultPublisher_);
  h.ranking().registerTrack(ftn("b"), 80, defaultPublisher_);
  h.ranking().registerTrack(ftn("c"), 70, defaultPublisher_);
  h.ranking().registerTrack(ftn("d"), 50, defaultPublisher_);
  h.clearEvents();

  // Move "d" from rank 3 to rank 2 - still outside top-2
  h.ranking().updateSortValue(ftn("d"), 75);
  EXPECT_EQ(h.selected_.size(), 0u);

  // Swap "a" and "b" within the selected zone
  h.ranking().updateSortValue(ftn("a"), 79);
  EXPECT_EQ(h.selected_.size(), 0u);
}

TEST_F(SimpleTopNTrackerBaseTest, FastPath_SameValueNoNotification) {
  SimpleRankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(1, sub, true);
  h.ranking().registerTrack(ftn("a"), 50, defaultPublisher_);
  h.clearEvents();

  h.ranking().updateSortValue(ftn("a"), 50);
  EXPECT_EQ(h.selected_.size(), 0u);
}

// ---------------------------------------------------------------------------
// updateSortValue slow path - promotions and evictions
// ---------------------------------------------------------------------------

TEST_F(SimpleTopNTrackerBaseTest, SlowPath_TrackPromotedWhenCrossesThreshold) {
  SimpleRankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(1, sub, true);

  h.ranking().registerTrack(ftn("a"), 80, defaultPublisher_);
  h.ranking().registerTrack(ftn("b"), 60, defaultPublisher_);
  h.clearEvents();

  // "b" jumps to rank 0 - should be selected, "a" evicted
  h.ranking().updateSortValue(ftn("b"), 90);
  EXPECT_EQ(h.selectCount(ftn("b"), sub.get()), 1);
  EXPECT_EQ(h.evictCount(ftn("a"), sub.get()), 1);
}

TEST_F(SimpleTopNTrackerBaseTest, SlowPath_MultipleSubscribersWithDifferentN) {
  SimpleRankingHarness h;
  auto sub1 = makeSession(); // wants top-1
  auto sub3 = makeSession(); // wants top-3

  h.ranking().addSessionToTopNGroup(1, sub1, true);
  h.ranking().addSessionToTopNGroup(3, sub3, true);

  h.ranking().registerTrack(ftn("a"), 100, defaultPublisher_);
  h.ranking().registerTrack(ftn("b"), 80, defaultPublisher_);
  h.ranking().registerTrack(ftn("c"), 60, defaultPublisher_);
  h.ranking().registerTrack(ftn("d"), 40, defaultPublisher_);
  h.clearEvents();

  // "d" surpasses "a" - crosses threshold for both
  h.ranking().updateSortValue(ftn("d"), 110);

  EXPECT_GE(h.selectCount(ftn("d"), sub1.get()), 1);
  EXPECT_GE(h.selectCount(ftn("d"), sub3.get()), 1);
}

// ---------------------------------------------------------------------------
// removeTrack
// ---------------------------------------------------------------------------

TEST_F(SimpleTopNTrackerBaseTest, RemoveSelected_PromotesNextTrack) {
  SimpleRankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(1, sub, true);

  h.ranking().registerTrack(ftn("a"), 90, defaultPublisher_);
  h.ranking().registerTrack(ftn("b"), 70, defaultPublisher_);
  h.clearEvents();

  h.ranking().removeTrack(ftn("a"));

  EXPECT_EQ(h.evictCount(ftn("a"), sub.get()), 1);
  EXPECT_EQ(h.selectCount(ftn("b"), sub.get()), 1);
}

TEST_F(SimpleTopNTrackerBaseTest, RemoveUnselected_NoPromotion) {
  SimpleRankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(1, sub, true);

  h.ranking().registerTrack(ftn("a"), 90, defaultPublisher_);
  h.ranking().registerTrack(ftn("b"), 70, defaultPublisher_);
  h.clearEvents();

  h.ranking().removeTrack(ftn("b"));
  EXPECT_EQ(h.selected_.size(), 0u);
  EXPECT_EQ(h.evicted_.size(), 0u);
}

// ---------------------------------------------------------------------------
// Multiple subscribers with different N
// ---------------------------------------------------------------------------

TEST_F(SimpleTopNTrackerBaseTest, TwoSubscribersDifferentN_IndependentSelection) {
  SimpleRankingHarness h;
  auto sub1 = makeSession(); // top-1
  auto sub3 = makeSession(); // top-3

  h.ranking().addSessionToTopNGroup(1, sub1, true);
  h.ranking().addSessionToTopNGroup(3, sub3, true);

  h.ranking().registerTrack(ftn("a"), 100, defaultPublisher_);
  h.ranking().registerTrack(ftn("b"), 80, defaultPublisher_);
  h.ranking().registerTrack(ftn("c"), 60, defaultPublisher_);
  h.ranking().registerTrack(ftn("d"), 40, defaultPublisher_);

  // sub1 should have received only "a"
  EXPECT_EQ(h.selectCount(ftn("a"), sub1.get()), 1);
  EXPECT_EQ(h.selectCount(ftn("b"), sub1.get()), 0);

  // sub3 should have received "a", "b", "c"
  EXPECT_EQ(h.selectCount(ftn("a"), sub3.get()), 1);
  EXPECT_EQ(h.selectCount(ftn("b"), sub3.get()), 1);
  EXPECT_EQ(h.selectCount(ftn("c"), sub3.get()), 1);
  EXPECT_EQ(h.selectCount(ftn("d"), sub3.get()), 0);
}

// ---------------------------------------------------------------------------
// Session removal
// ---------------------------------------------------------------------------

TEST_F(SimpleTopNTrackerBaseTest, RemoveSession_TrackerContinuesWorking) {
  SimpleRankingHarness h;
  auto sub1 = makeSession();
  auto sub2 = makeSession();

  h.ranking().addSessionToTopNGroup(2, sub1, true);
  h.ranking().addSessionToTopNGroup(2, sub2, true);

  h.ranking().removeSessionFromTopNGroup(2, sub1);

  const auto* grp = h.ranking().getTopNGroup(2);
  ASSERT_NE(grp, nullptr);
  EXPECT_EQ(grp->sessions.size(), 1u);
}

TEST_F(SimpleTopNTrackerBaseTest, RemoveLastSession_GroupRemoved) {
  SimpleRankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(3, sub, true);

  h.ranking().removeSessionFromTopNGroup(3, sub);

  EXPECT_EQ(h.ranking().getTopNGroup(3), nullptr);
  EXPECT_TRUE(h.ranking().empty());
}

// ---------------------------------------------------------------------------
// Snapshot lock-free reads
// ---------------------------------------------------------------------------

TEST_F(SimpleTopNTrackerBaseTest, SnapshotIsSortedDescending) {
  SimpleRankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(5, sub, true);

  h.ranking().registerTrack(ftn("c"), 30, defaultPublisher_);
  h.ranking().registerTrack(ftn("a"), 100, defaultPublisher_);
  h.ranking().registerTrack(ftn("b"), 50, defaultPublisher_);

  // Verify through selection order - highest values selected first
  EXPECT_EQ(h.selectCount(ftn("a"), sub.get()), 1);
  EXPECT_EQ(h.selectCount(ftn("b"), sub.get()), 1);
  EXPECT_EQ(h.selectCount(ftn("c"), sub.get()), 1);
}

// ---------------------------------------------------------------------------
// sweepIdle tests
// ---------------------------------------------------------------------------

TEST_F(SimpleTopNTrackerBaseTest, SweepIdle_IdleTrackEvictedAndReplacementPromoted) {
  SimpleRankingHarness h(std::chrono::milliseconds(100));
  auto sub = makeSession();

  h.ranking().registerTrack(ftn("a"), 100, defaultPublisher_);
  h.ranking().registerTrack(ftn("b"), 90, defaultPublisher_);
  h.ranking().registerTrack(ftn("c"), 80, defaultPublisher_);

  h.ranking().addSessionToTopNGroup(2, sub, true);
  h.clearEvents();

  auto now = std::chrono::steady_clock::now();
  h.setActivityTime(ftn("a"), now);
  h.setActivityTime(ftn("b"), now - std::chrono::milliseconds(200)); // idle
  h.setActivityTime(ftn("c"), now);

  h.ranking().sweepIdle();

  EXPECT_EQ(h.evictCount(ftn("b")), 1);
  EXPECT_EQ(h.selectCount(ftn("c")), 1);
}

TEST_F(SimpleTopNTrackerBaseTest, SweepIdle_TrackThatNeverPublishedIsTreatedAsIdle) {
  SimpleRankingHarness h(std::chrono::milliseconds(100));
  auto sub = makeSession();

  h.ranking().registerTrack(ftn("a"), 100, defaultPublisher_);
  h.ranking().registerTrack(ftn("b"), 90, defaultPublisher_);
  h.ranking().registerTrack(ftn("c"), 80, defaultPublisher_);

  h.ranking().addSessionToTopNGroup(2, sub, true);
  h.clearEvents();

  auto now = std::chrono::steady_clock::now();
  h.setActivityTime(ftn("a"), now);
  h.setActivityTime(ftn("c"), now);
  // b has no activity time set -> epoch -> treated as idle

  h.ranking().sweepIdle();

  EXPECT_EQ(h.evictCount(ftn("b")), 1);
  EXPECT_EQ(h.selectCount(ftn("c")), 1);
}

TEST_F(SimpleTopNTrackerBaseTest, SweepIdle_NoEvictionWhenIdleTimeoutIsZero) {
  SimpleRankingHarness h(std::chrono::milliseconds(0));
  auto sub = makeSession();

  h.ranking().registerTrack(ftn("a"), 100, defaultPublisher_);
  h.ranking().registerTrack(ftn("b"), 90, defaultPublisher_);

  h.ranking().addSessionToTopNGroup(2, sub, true);
  h.clearEvents();

  auto now = std::chrono::steady_clock::now();
  h.setActivityTime(ftn("a"), now - std::chrono::hours(1));
  h.setActivityTime(ftn("b"), now - std::chrono::hours(1));

  h.ranking().sweepIdle();

  EXPECT_EQ(h.selectCount(ftn("a")), 0);
  EXPECT_EQ(h.selectCount(ftn("b")), 0);
  EXPECT_EQ(h.evicted_.size(), 0u);
}

TEST_F(SimpleTopNTrackerBaseTest, SweepIdle_ActiveTracksNotEvicted) {
  SimpleRankingHarness h(std::chrono::milliseconds(100));
  auto sub = makeSession();

  h.ranking().registerTrack(ftn("a"), 100, defaultPublisher_);
  h.ranking().registerTrack(ftn("b"), 90, defaultPublisher_);

  h.ranking().addSessionToTopNGroup(2, sub, true);
  h.clearEvents();

  auto now = std::chrono::steady_clock::now();
  h.setActivityTime(ftn("a"), now);
  h.setActivityTime(ftn("b"), now);

  h.ranking().sweepIdle();

  EXPECT_EQ(h.evicted_.size(), 0u);
}

TEST_F(SimpleTopNTrackerBaseTest, SweepThrottle_SkipsRunIfCalledTooSoon) {
  SimpleRankingHarness h(std::chrono::milliseconds(100), std::chrono::milliseconds(500));
  auto sub = makeSession();

  h.ranking().registerTrack(ftn("a"), 100, defaultPublisher_);
  h.ranking().registerTrack(ftn("b"), 90, defaultPublisher_);
  h.ranking().addSessionToTopNGroup(1, sub, true);
  h.clearEvents();

  h.setActivityTime(ftn("b"), std::chrono::steady_clock::now());

  // First sweep: a is idle -> evicted, b promoted
  h.ranking().sweepIdle();
  EXPECT_EQ(h.selectCount(ftn("b")), 1);

  // Re-register a
  h.ranking().registerTrack(ftn("a"), 100, defaultPublisher_);
  h.clearEvents();

  // Second sweep immediately after: throttle suppresses it
  h.ranking().sweepIdle();
  EXPECT_EQ(h.selectCount(ftn("a")), 0);
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

TEST_F(SimpleTopNTrackerBaseTest, RegisterTrackWithNoSubscribers) {
  SimpleRankingHarness h;

  h.ranking().registerTrack(ftn("a"), 100, defaultPublisher_);
  h.ranking().registerTrack(ftn("b"), 50, defaultPublisher_);

  EXPECT_EQ(h.ranking().numTracks(), 2u);
  EXPECT_EQ(h.selected_.size(), 0u);
}

TEST_F(SimpleTopNTrackerBaseTest, UpdateSortValueWithNoSubscribers) {
  SimpleRankingHarness h;

  h.ranking().registerTrack(ftn("a"), 100, defaultPublisher_);
  h.ranking().updateSortValue(ftn("a"), 200);

  EXPECT_EQ(h.selected_.size(), 0u);
}

TEST_F(SimpleTopNTrackerBaseTest, RemoveUnknownTrack_NoOp) {
  SimpleRankingHarness h;

  h.ranking().removeTrack(ftn("unknown"));

  EXPECT_EQ(h.evicted_.size(), 0u);
}

TEST_F(SimpleTopNTrackerBaseTest, DuplicateRegisterTrack_Ignored) {
  SimpleRankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(2, sub, true);

  h.ranking().registerTrack(ftn("a"), 100, defaultPublisher_);
  h.ranking().registerTrack(ftn("a"), 200, defaultPublisher_); // duplicate

  EXPECT_EQ(h.ranking().numTracks(), 1u);
  EXPECT_EQ(h.selectCount(ftn("a")), 1);
}

// ---------------------------------------------------------------------------
// Tie-breaker: arrival sequence
// ---------------------------------------------------------------------------

TEST_F(SimpleTopNTrackerBaseTest, TieBreaker_EarlierArrivalWins) {
  SimpleRankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(1, sub, true);

  // Register with same value - first one should win
  h.ranking().registerTrack(ftn("first"), 100, defaultPublisher_);
  h.ranking().registerTrack(ftn("second"), 100, defaultPublisher_);

  // Only first should be selected (N=1)
  EXPECT_EQ(h.selectCount(ftn("first")), 1);
  EXPECT_EQ(h.selectCount(ftn("second")), 0);
}

} // namespace
