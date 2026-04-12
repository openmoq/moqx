/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * Unit tests for PropertyRanking top-N base logic (no self-exclusion).
 *
 * Covers:
 * - registerTrack + addSessionToTopNGroup → correct sessions notified at top-N boundary
 * - updateSortValue fast path: no notification when threshold not crossed
 * - updateSortValue slow path: correct promotions/evictions across multiple
 *   TopNGroups with different N
 * - removeTrack of a selected track → next eligible track promoted via
 *   deselected queue, then ranked list
 * - Multiple TopNGroups (N=1, N=3) on same ranking instance
 * - deselectedQueue bounded by maxDeselected_; eviction callback fires when
 *   queue overflows
 * - Session removal cleans up TopNGroup; last session removes the group
 */

#include "relay/PropertyRanking.h"

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

// Thin wrapper: creates a PropertyRanking, records callbacks in vectors.
class RankingHarness {
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

  explicit RankingHarness(
      uint64_t maxDeselected = 5,
      std::chrono::milliseconds idleTimeout = std::chrono::milliseconds(0)
  )
      : ranking_(std::make_unique<PropertyRanking>(
            kProp,
            maxDeselected,
            idleTimeout,
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

  // Set a mock activity time for a track (used in idle tests)
  void setActivityTime(const FullTrackName& f, std::chrono::steady_clock::time_point t) {
    activityTimes_[f] = t;
  }

  PropertyRanking& ranking() { return *ranking_; }

  // Count how many select events were fired for a given track
  int selectCount(const FullTrackName& f) const {
    int n = 0;
    for (auto& e : selected_) {
      if (e.ftn == f) {
        n++;
      }
    }
    return n;
  }

  // Count select events for a (track, session) pair
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

  void clearEvents() {
    selected_.clear();
    evicted_.clear();
  }

  std::vector<SelectEvent> selected_;
  std::vector<EvictEvent> evicted_;
  folly::F14FastMap<FullTrackName, std::chrono::steady_clock::time_point, FullTrackName::hash>
      activityTimes_;

private:
  std::unique_ptr<PropertyRanking> ranking_;
};

// Minimal stub session (not a real MoQSession, just needs a unique address)
// We use NiceMock<MockMoQSession> from moxygen mocks but that requires an
// executor. Instead, we make shared_ptrs to distinct mock objects so that
// the F14FastMap keys are unique pointers.
//
// PropertyRanking only stores shared_ptr<MoQSession> as map keys and passes
// them back through callbacks — it never calls any MoQSession methods itself.
// So we can cast a shared_ptr<MockMoQSession> (which IS a MoQSession) safely.

class PropertyRankingBaseTest : public ::testing::Test {
protected:
  // PropertyRanking only stores shared_ptr<MoQSession> as map keys and passes
  // them back through callbacks — it never calls any MoQSession methods.
  // MockMoQSession has a default constructor for exactly this use case.
  std::shared_ptr<MoQSession> makeSession() {
    auto s = std::make_shared<NiceMock<moxygen::test::MockMoQSession>>();
    sessions_.push_back(s);
    return std::static_pointer_cast<MoQSession>(s);
  }

  std::vector<std::shared_ptr<NiceMock<moxygen::test::MockMoQSession>>> sessions_;
};

// ---------------------------------------------------------------------------
// registerTrack + addSessionToTopNGroup
// ---------------------------------------------------------------------------

TEST_F(PropertyRankingBaseTest, RegisterThenSubscribe_TopNBoundary) {
  RankingHarness h;

  // 3 tracks registered before any subscriber
  h.ranking().registerTrack(ftn("a"), 90, {});
  h.ranking().registerTrack(ftn("b"), 80, {});
  h.ranking().registerTrack(ftn("c"), 70, {});
  h.ranking().registerTrack(ftn("d"), 20, {});

  // Subscriber joins wanting top-2
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(2, sub, /*forward=*/true);

  // Should have received exactly ftn("a") and ftn("b")
  EXPECT_EQ(h.selectCount(ftn("a"), sub.get()), 1);
  EXPECT_EQ(h.selectCount(ftn("b"), sub.get()), 1);
  EXPECT_EQ(h.selectCount(ftn("c"), sub.get()), 0);
  EXPECT_EQ(h.selectCount(ftn("d"), sub.get()), 0);
}

TEST_F(PropertyRankingBaseTest, SubscribeThenRegister_TopNBoundary) {
  RankingHarness h;

  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(2, sub, /*forward=*/true);

  // Tracks arrive after subscriber
  h.ranking().registerTrack(ftn("a"), 90, {});
  h.ranking().registerTrack(ftn("b"), 80, {});
  h.ranking().registerTrack(ftn("c"), 70, {}); // rank 2 — not in top-2

  EXPECT_EQ(h.selectCount(ftn("a"), sub.get()), 1);
  EXPECT_EQ(h.selectCount(ftn("b"), sub.get()), 1);
  EXPECT_EQ(h.selectCount(ftn("c"), sub.get()), 0);
}

TEST_F(PropertyRankingBaseTest, ForwardFlagPassedThrough_False) {
  RankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(1, sub, /*forward=*/false);
  h.ranking().registerTrack(ftn("a"), 10, {});

  ASSERT_EQ(h.selected_.size(), 1u);
  EXPECT_FALSE(h.selected_[0].forward);
}

TEST_F(PropertyRankingBaseTest, ForwardFlagPassedThrough_True) {
  RankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(1, sub, /*forward=*/true);
  h.ranking().registerTrack(ftn("a"), 10, {});

  ASSERT_EQ(h.selected_.size(), 1u);
  EXPECT_TRUE(h.selected_[0].forward);
}

// ---------------------------------------------------------------------------
// updateSortValue fast path
// ---------------------------------------------------------------------------

TEST_F(PropertyRankingBaseTest, FastPath_NoNotificationWhenThresholdNotCrossed) {
  RankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(2, sub, true);

  // 4 tracks: top-2 = {a=90, b=80}; {c=70, d=50} are outside
  h.ranking().registerTrack(ftn("a"), 90, {});
  h.ranking().registerTrack(ftn("b"), 80, {});
  h.ranking().registerTrack(ftn("c"), 70, {}); // rank 2 — just outside top-2
  h.ranking().registerTrack(ftn("d"), 50, {}); // rank 3
  h.clearEvents();

  // Move "d" from rank 3 to rank 2 — still outside top-2, no notification
  h.ranking().updateSortValue(ftn("d"), 75); // d=75 < c=70? no: 75>70 → d is rank 2, c rank 3
  EXPECT_EQ(h.selected_.size(), 0u);

  // Swap "a" and "b" within the selected zone — both stay in top-2
  h.ranking().updateSortValue(ftn("a"), 79); // a=79 < b=80 → a is rank 1, still in top-2
  EXPECT_EQ(h.selected_.size(), 0u);
}

TEST_F(PropertyRankingBaseTest, FastPath_SameValueNoNotification) {
  RankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(1, sub, true);
  h.ranking().registerTrack(ftn("a"), 50, {});
  h.clearEvents();

  h.ranking().updateSortValue(ftn("a"), 50); // no change
  EXPECT_EQ(h.selected_.size(), 0u);
}

// ---------------------------------------------------------------------------
// updateSortValue slow path — promotions and evictions
// ---------------------------------------------------------------------------

TEST_F(PropertyRankingBaseTest, SlowPath_TrackPromotedWhenCrossesThreshold) {
  RankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(1, sub, true);

  h.ranking().registerTrack(ftn("a"), 80, {});
  h.ranking().registerTrack(ftn("b"), 60, {}); // rank 1 — not in top-1
  h.clearEvents();

  // "b" jumps to rank 0 — should be selected, "a" deselected
  h.ranking().updateSortValue(ftn("b"), 90);
  EXPECT_EQ(h.selectCount(ftn("b"), sub.get()), 1);
  EXPECT_EQ(h.selectCount(ftn("a"), sub.get()), 0);
}

TEST_F(PropertyRankingBaseTest, SlowPath_MultipleTopNGroups) {
  RankingHarness h;
  auto sub1 = makeSession(); // wants top-1
  auto sub3 = makeSession(); // wants top-3

  h.ranking().addSessionToTopNGroup(1, sub1, true);
  h.ranking().addSessionToTopNGroup(3, sub3, true);

  h.ranking().registerTrack(ftn("a"), 100, {});
  h.ranking().registerTrack(ftn("b"), 80, {});
  h.ranking().registerTrack(ftn("c"), 60, {});
  h.ranking().registerTrack(ftn("d"), 40, {}); // rank 3 — NOT in top-3, not in top-1
  h.clearEvents();

  // "d" surpasses "a" — crosses threshold for both groups
  h.ranking().updateSortValue(ftn("d"), 110);

  // sub1 (top-1) should now receive "d"
  EXPECT_GE(h.selectCount(ftn("d"), sub1.get()), 1);
  // sub3 (top-3) should also receive "d" — it was NOT in top-3 before (rank 3),
  // now it's rank 0 so sub3 should be notified
  EXPECT_GE(h.selectCount(ftn("d"), sub3.get()), 1);
}

// ---------------------------------------------------------------------------
// removeTrack of a selected track — promotion via deselected queue / ranked list
// ---------------------------------------------------------------------------

TEST_F(PropertyRankingBaseTest, RemoveSelected_PromotesFromDeselectedQueue) {
  // To test deselected-queue promotion, we need a track to have been selected,
  // then fall out of selection (entering the queue), then be promoted on removeTrack.
  //
  // Setup: top-1, tracks a=90(selected), b=80
  // Step 1: c registers at 95 — c→rank0(selected), a→rank1(deselected, enters queue)
  // Step 2: remove c — a should be promoted from deselected queue
  RankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(1, sub, true);

  h.ranking().registerTrack(ftn("a"), 90, {}); // selected
  h.ranking().registerTrack(ftn("b"), 80, {}); // rank 1, unselected
  h.clearEvents();

  // c arrives louder than a → a falls to deselected queue
  h.ranking().registerTrack(ftn("c"), 95, {}); // c→rank0(selected), a→rank1(deselected queue)
  h.clearEvents();

  // Remove c (selected): a should be promoted from deselected queue
  h.ranking().removeTrack(ftn("c"));
  EXPECT_EQ(h.selectCount(ftn("a"), sub.get()), 1);
  EXPECT_EQ(h.selectCount(ftn("b"), sub.get()), 0);
}

TEST_F(PropertyRankingBaseTest, RemoveSelected_PromotesFromRankedList) {
  RankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(1, sub, true);

  h.ranking().registerTrack(ftn("a"), 90, {});
  h.ranking().registerTrack(ftn("b"), 70, {}); // rank 1
  h.clearEvents();

  h.ranking().removeTrack(ftn("a"));
  EXPECT_EQ(h.selectCount(ftn("b"), sub.get()), 1);
}

TEST_F(PropertyRankingBaseTest, RemoveDeselected_NoPromotionNeeded) {
  RankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(1, sub, true);

  h.ranking().registerTrack(ftn("a"), 90, {});
  h.ranking().registerTrack(ftn("b"), 70, {}); // rank 1 — deselected
  h.clearEvents();

  // Removing a deselected track should not trigger any selection notification
  h.ranking().removeTrack(ftn("b"));
  EXPECT_EQ(h.selected_.size(), 0u);
}

// ---------------------------------------------------------------------------
// Multiple TopNGroups with different N
// ---------------------------------------------------------------------------

TEST_F(PropertyRankingBaseTest, TwoGroupsDifferentN_IndependentSelection) {
  RankingHarness h;
  auto sub1 = makeSession(); // top-1
  auto sub3 = makeSession(); // top-3

  h.ranking().addSessionToTopNGroup(1, sub1, true);
  h.ranking().addSessionToTopNGroup(3, sub3, true);

  h.ranking().registerTrack(ftn("a"), 100, {});
  h.ranking().registerTrack(ftn("b"), 80, {});
  h.ranking().registerTrack(ftn("c"), 60, {});
  h.ranking().registerTrack(ftn("d"), 40, {});

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
// deselectedQueue bounded by maxDeselected_
// ---------------------------------------------------------------------------

TEST_F(PropertyRankingBaseTest, DeselectedQueueBounded_EvictionFires) {
  // Only tracks that were previously *selected* enter the deselected queue.
  // To overflow a queue with maxDeselected=2, we need 3 different tracks to
  // have been selected at some point and then fall out.
  //
  // Strategy: top-1, rotate the "winner" 3 times so each loser enters the queue.
  //   round 1: a=90 selected. Register b=95 → b selected, a enters queue (size 1).
  //   round 2: c=100 registered → c selected, b enters queue (size 2, at limit).
  //   round 3: d=110 registered → d selected, c enters queue (size 3 > max=2 → eviction).
  RankingHarness h(/*maxDeselected=*/2);
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(1, sub, true);

  h.ranking().registerTrack(ftn("a"), 90, {});  // selected
  h.ranking().registerTrack(ftn("b"), 95, {});  // b→selected, a→deselected queue (size 1)
  h.ranking().registerTrack(ftn("c"), 100, {}); // c→selected, b→deselected queue (size 2)
  EXPECT_EQ(h.evicted_.size(), 0u);             // not yet

  h.ranking().registerTrack(ftn("d"), 110, {}); // d→selected, c→deselected queue (size 3 > 2)
  EXPECT_GE(h.evicted_.size(), 1u);             // oldest entry evicted
}

// ---------------------------------------------------------------------------
// Session removal
// ---------------------------------------------------------------------------

TEST_F(PropertyRankingBaseTest, RemoveSession_GroupRemainsIfOtherSessionsExist) {
  RankingHarness h;
  auto sub1 = makeSession();
  auto sub2 = makeSession();

  h.ranking().addSessionToTopNGroup(2, sub1, true);
  h.ranking().addSessionToTopNGroup(2, sub2, true);

  h.ranking().removeSessionFromTopNGroup(2, sub1);

  const auto* grp = h.ranking().getTopNGroup(2);
  ASSERT_NE(grp, nullptr);
  EXPECT_EQ(grp->sessions.size(), 1u);
}

TEST_F(PropertyRankingBaseTest, RemoveLastSession_GroupRemoved) {
  RankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(3, sub, true);

  h.ranking().removeSessionFromTopNGroup(3, sub);

  EXPECT_EQ(h.ranking().getTopNGroup(3), nullptr);
  EXPECT_TRUE(h.ranking().empty());
}

TEST_F(PropertyRankingBaseTest, RemoveLastSession_ThresholdUpdated) {
  RankingHarness h;
  auto sub1 = makeSession();
  auto sub2 = makeSession();

  h.ranking().addSessionToTopNGroup(1, sub1, true);
  h.ranking().addSessionToTopNGroup(3, sub2, true);

  h.ranking().removeSessionFromTopNGroup(3, sub2); // group N=3 gone

  // Only N=1 group remains; threshold should reflect that
  EXPECT_NE(h.ranking().getTopNGroup(1), nullptr);
  EXPECT_EQ(h.ranking().getTopNGroup(3), nullptr);
}

// ---------------------------------------------------------------------------
// recomputeTopNGroups: track crosses one group threshold but not another
// ---------------------------------------------------------------------------

// TODO: Add tests for deselection/reselection notifications once onDeselected
// and onReselected callbacks are implemented (see PropertyRanking.h TODO).
// Tests needed:
// - DeselectionNotification_WhenTrackFallsOutOfTopN
// - ReselectionNotification_WhenPromotedFromQueue

// This test validates that TopNGroups are evaluated independently: crossing
// one N boundary doesn't affect groups where the track was already selected.
TEST_F(PropertyRankingBaseTest, SlowPath_CrossesLowerThreshold_HigherGroupUnchanged) {
  // N=2 group and N=4 group. A track moves from rank 3 (inside N=4, outside
  // N=2) to rank 1 (inside both). Only the N=2 group should fire a selection
  // notification; the N=4 group's state is unchanged (track was already
  // selected there).
  RankingHarness h;
  auto sub2 = makeSession(); // top-2
  auto sub4 = makeSession(); // top-4

  h.ranking().addSessionToTopNGroup(2, sub2, true);
  h.ranking().addSessionToTopNGroup(4, sub4, true);

  h.ranking().registerTrack(ftn("a"), 100, {}); // rank 0
  h.ranking().registerTrack(ftn("b"), 80, {});  // rank 1
  h.ranking().registerTrack(ftn("c"), 60, {});  // rank 2
  h.ranking().registerTrack(ftn("d"), 40, {});  // rank 3 — in top-4, not top-2
  h.clearEvents();

  // d jumps to rank 1: crosses the N=2 boundary (3→1), stays inside N=4
  h.ranking().updateSortValue(ftn("d"), 90); // order: a=100, d=90, b=80, c=60

  // sub2 should be notified about d entering top-2
  EXPECT_EQ(h.selectCount(ftn("d"), sub2.get()), 1);
  // sub4 should NOT be notified — d was already in its top-4
  EXPECT_EQ(h.selectCount(ftn("d"), sub4.get()), 0);
  // b was at rank 1 (selected for both), falls to rank 2: leaves top-2
  // sub2 gets no additional select events (b was demoted, not selected)
  EXPECT_EQ(h.selectCount(ftn("b"), sub2.get()), 0);
}

// ---------------------------------------------------------------------------
// removeTrack fallback: ranked list scan promotes highest non-selected track
// ---------------------------------------------------------------------------

TEST_F(PropertyRankingBaseTest, RemoveSelected_FallbackPicksHighestNonSelected) {
  // top-2, tracks a=100(sel), b=80(sel), c=60, d=40
  // Remove a: fallback scan should promote c (rank 2, highest non-selected),
  // not d (rank 3).
  RankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(2, sub, true);

  h.ranking().registerTrack(ftn("a"), 100, {});
  h.ranking().registerTrack(ftn("b"), 80, {});
  h.ranking().registerTrack(ftn("c"), 60, {});
  h.ranking().registerTrack(ftn("d"), 40, {});
  h.clearEvents();

  h.ranking().removeTrack(ftn("a"));
  EXPECT_EQ(h.selectCount(ftn("c"), sub.get()), 1);
  EXPECT_EQ(h.selectCount(ftn("d"), sub.get()), 0);
  EXPECT_EQ(h.selectCount(ftn("b"), sub.get()), 0); // b was already selected
}

// ---------------------------------------------------------------------------
// Two-phase algorithm: multiple adjacent groups handled in single traversal
// ---------------------------------------------------------------------------

TEST_F(PropertyRankingBaseTest, MultipleAdjacentGroups_SingleTraversalDemotion) {
  // Tests that registerTrack correctly demotes tracks at multiple boundary positions
  // in a single traversal when multiple TopNGroups have adjacent N values.
  RankingHarness h;
  auto sub2 = makeSession(); // top-2
  auto sub3 = makeSession(); // top-3
  auto sub4 = makeSession(); // top-4

  h.ranking().addSessionToTopNGroup(2, sub2, true);
  h.ranking().addSessionToTopNGroup(3, sub3, true);
  h.ranking().addSessionToTopNGroup(4, sub4, true);

  // Register tracks at positions 0,1,2,3
  h.ranking().registerTrack(ftn("a"), 100, {}); // rank 0
  h.ranking().registerTrack(ftn("b"), 80, {});  // rank 1
  h.ranking().registerTrack(ftn("c"), 60, {});  // rank 2
  h.ranking().registerTrack(ftn("d"), 40, {});  // rank 3
  h.clearEvents();

  // Register a track that becomes rank 0, pushing everyone down
  // This should trigger demotions at positions 2, 3, 4 for groups N=2, N=3, N=4
  h.ranking().registerTrack(ftn("e"), 110, {}); // e becomes rank 0

  // e should be selected for all three groups
  EXPECT_EQ(h.selectCount(ftn("e"), sub2.get()), 1);
  EXPECT_EQ(h.selectCount(ftn("e"), sub3.get()), 1);
  EXPECT_EQ(h.selectCount(ftn("e"), sub4.get()), 1);

  // After e enters:
  // - sub2 (top-2): e, a selected; b demoted at position 2
  // - sub3 (top-3): e, a, b selected; c demoted at position 3
  // - sub4 (top-4): e, a, b, c selected; d demoted at position 4
  // Note: b was selected for sub2 initially, now demoted
  //       c was selected for sub3 initially, now demoted
  //       d was selected for sub4 initially, now demoted
}

TEST_F(PropertyRankingBaseTest, MultipleAdjacentGroups_SingleTraversalPromotion) {
  // Tests that recomputeTopNGroups correctly promotes tracks at multiple boundary
  // positions in a single traversal when a track falls out of multiple groups.
  RankingHarness h;
  auto sub2 = makeSession(); // top-2
  auto sub3 = makeSession(); // top-3
  auto sub4 = makeSession(); // top-4

  h.ranking().addSessionToTopNGroup(2, sub2, true);
  h.ranking().addSessionToTopNGroup(3, sub3, true);
  h.ranking().addSessionToTopNGroup(4, sub4, true);

  // Register tracks: a=100, b=80, c=60, d=40, e=20
  h.ranking().registerTrack(ftn("a"), 100, {}); // rank 0
  h.ranking().registerTrack(ftn("b"), 80, {});  // rank 1
  h.ranking().registerTrack(ftn("c"), 60, {});  // rank 2
  h.ranking().registerTrack(ftn("d"), 40, {});  // rank 3
  h.ranking().registerTrack(ftn("e"), 20, {});  // rank 4
  h.clearEvents();

  // Drop "a" to rank 4 (value=10), falling out of all three groups
  // This should promote tracks at positions 1, 2, 3 for the respective groups
  h.ranking().updateSortValue(ftn("a"), 10);

  // After a drops: order is b=80(0), c=60(1), d=40(2), e=20(3), a=10(4)
  // - sub2 (top-2): b, c selected (c promoted at position 1)
  // - sub3 (top-3): b, c, d selected (d promoted at position 2)
  // - sub4 (top-4): b, c, d, e selected (e promoted at position 3)
  EXPECT_GE(h.selectCount(ftn("c"), sub2.get()), 1); // c promoted into top-2
  EXPECT_GE(h.selectCount(ftn("d"), sub3.get()), 1); // d promoted into top-3
  EXPECT_GE(h.selectCount(ftn("e"), sub4.get()), 1); // e promoted into top-4
}

// ---------------------------------------------------------------------------
// selectionThreshold changes: cache invalidation on increase, stability on decrease
// ---------------------------------------------------------------------------

TEST_F(PropertyRankingBaseTest, ThresholdIncrease_CacheInvalidatedForNewRange) {
  // Tests that when a larger N group is added, tracks that previously had
  // sentinel ranks (UINT64_MAX) get accurate ranks after cache rebuild.
  //
  // Scenario: Start with N=2, add tracks, then add N=5 group. A track at
  // position 3 (which had sentinel rank) should correctly detect threshold
  // crossing when its value changes.
  RankingHarness h;
  auto sub2 = makeSession();
  h.ranking().addSessionToTopNGroup(2, sub2, true);

  // Register 5 tracks: positions 0-4
  h.ranking().registerTrack(ftn("a"), 100, {}); // rank 0 - selected for N=2
  h.ranking().registerTrack(ftn("b"), 80, {});  // rank 1 - selected for N=2
  h.ranking().registerTrack(ftn("c"), 60, {});  // rank 2 - outside N=2, has sentinel
  h.ranking().registerTrack(ftn("d"), 40, {});  // rank 3 - outside N=2, has sentinel
  h.ranking().registerTrack(ftn("e"), 20, {});  // rank 4 - outside N=2, has sentinel
  h.clearEvents();

  // Add a larger N group - this should invalidate cache
  auto sub5 = makeSession();
  h.ranking().addSessionToTopNGroup(5, sub5, true);
  h.clearEvents();

  // Now update "d" (was at rank 3 with sentinel) to jump into top-2
  // If cache wasn't invalidated, d's oldRank would be UINT64_MAX and
  // recomputeTopNGroups would incorrectly think d wasn't in top-5 before.
  h.ranking().updateSortValue(ftn("d"), 110); // d becomes rank 0

  // d should be selected for sub2 (entered top-2)
  EXPECT_GE(h.selectCount(ftn("d"), sub2.get()), 1);

  // d should NOT trigger a new selection for sub5 - it was already in top-5
  // before the update (at rank 3). If cache was stale, this would incorrectly
  // fire because oldRank=UINT64_MAX would make wasInTopN=false.
  // After cache invalidation, oldRank=3 correctly makes wasInTopN=true for N=5.
  EXPECT_EQ(h.selectCount(ftn("d"), sub5.get()), 0);
}

TEST_F(PropertyRankingBaseTest, ThresholdDecrease_CachedRanksRemainValid) {
  // Tests that when a group is removed and threshold decreases, existing
  // cached ranks (which are more than needed) remain valid.
  RankingHarness h;
  auto sub2 = makeSession();
  auto sub5 = makeSession();

  h.ranking().addSessionToTopNGroup(5, sub5, true);
  h.ranking().addSessionToTopNGroup(2, sub2, true);

  // Register tracks
  h.ranking().registerTrack(ftn("a"), 100, {}); // rank 0
  h.ranking().registerTrack(ftn("b"), 80, {});  // rank 1
  h.ranking().registerTrack(ftn("c"), 60, {});  // rank 2
  h.ranking().registerTrack(ftn("d"), 40, {});  // rank 3
  h.ranking().registerTrack(ftn("e"), 20, {});  // rank 4
  h.clearEvents();

  // Remove the larger group - threshold decreases
  h.ranking().removeSessionFromTopNGroup(5, sub5);
  h.clearEvents();

  // Update a track that was in top-5 but outside top-2
  // With threshold decreased, this track might have more cached rank than
  // needed, but operations should still work correctly.
  h.ranking().updateSortValue(ftn("c"), 110); // c becomes rank 0, enters top-2

  EXPECT_GE(h.selectCount(ftn("c"), sub2.get()), 1);
}

TEST_F(PropertyRankingBaseTest, ThresholdIncrease_MultipleGroupsAddedSequentially) {
  // Tests adding groups with increasing N values sequentially.
  RankingHarness h;

  // Start with N=1
  auto sub1 = makeSession();
  h.ranking().addSessionToTopNGroup(1, sub1, true);

  h.ranking().registerTrack(ftn("a"), 100, {}); // rank 0
  h.ranking().registerTrack(ftn("b"), 80, {});  // rank 1
  h.ranking().registerTrack(ftn("c"), 60, {});  // rank 2
  h.clearEvents();

  // Add N=2 - threshold increases
  auto sub2 = makeSession();
  h.ranking().addSessionToTopNGroup(2, sub2, true);
  h.clearEvents();

  // Add N=3 - threshold increases again
  auto sub3 = makeSession();
  h.ranking().addSessionToTopNGroup(3, sub3, true);
  h.clearEvents();

  // Move "c" from rank 2 to rank 0
  h.ranking().updateSortValue(ftn("c"), 110);

  // c enters top-1, top-2 (was at rank 2, outside both)
  // c was already in top-3 (rank 2 < 3), so sub3 should NOT be notified
  EXPECT_GE(h.selectCount(ftn("c"), sub1.get()), 1);
  EXPECT_GE(h.selectCount(ftn("c"), sub2.get()), 1);
  EXPECT_EQ(h.selectCount(ftn("c"), sub3.get()), 0); // was already in top-3
}

// ---------------------------------------------------------------------------
// sweepIdle tests
// ---------------------------------------------------------------------------

TEST(PropertyRankingSweepIdle, IdleTrackEvictedAndReplacementPromoted) {
  // Setup: 3 tracks, N=2 group, idleTimeout=100ms
  RankingHarness h(5, std::chrono::milliseconds(100));
  auto sub = makeSession();

  h.ranking().registerTrack(ftn("a"), 100, {}); // rank 0
  h.ranking().registerTrack(ftn("b"), 90, {});  // rank 1
  h.ranking().registerTrack(ftn("c"), 80, {});  // rank 2

  h.ranking().addSessionToTopNGroup(2, sub, true);
  h.clearEvents();

  // Set activity times: a and c are active (recent), b is idle (old)
  auto now = std::chrono::steady_clock::now();
  h.setActivityTime(ftn("a"), now);
  h.setActivityTime(ftn("b"), now - std::chrono::milliseconds(200)); // idle
  h.setActivityTime(ftn("c"), now);

  // Sweep should evict b (idle) and promote c (next best)
  h.ranking().sweepIdle();

  // c should be promoted to replace b
  EXPECT_EQ(h.selectCount(ftn("c")), 1);

  // Check states: a and c selected, b deselected
  auto* group = h.ranking().getTopNGroup(2);
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(group->trackStates.at(ftn("a")), TrackState::Selected);
  EXPECT_EQ(group->trackStates.at(ftn("b")), TrackState::Deselected);
  EXPECT_EQ(group->trackStates.at(ftn("c")), TrackState::Selected);
}

TEST(PropertyRankingSweepIdle, TrackThatNeverPublishedIsTreatedAsIdle) {
  // A track with epoch timestamp (never published) should be evicted
  RankingHarness h(5, std::chrono::milliseconds(100));
  auto sub = makeSession();

  h.ranking().registerTrack(ftn("a"), 100, {}); // rank 0
  h.ranking().registerTrack(ftn("b"), 90, {});  // rank 1 - never sets activity time
  h.ranking().registerTrack(ftn("c"), 80, {});  // rank 2

  h.ranking().addSessionToTopNGroup(2, sub, true);
  h.clearEvents();

  // Only set activity for a and c; b has epoch (never published)
  auto now = std::chrono::steady_clock::now();
  h.setActivityTime(ftn("a"), now);
  h.setActivityTime(ftn("c"), now);
  // b has no activity time set -> epoch -> treated as idle

  h.ranking().sweepIdle();

  // b should be evicted, c promoted
  EXPECT_EQ(h.selectCount(ftn("c")), 1);

  auto* group = h.ranking().getTopNGroup(2);
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(group->trackStates.at(ftn("b")), TrackState::Deselected);
  EXPECT_EQ(group->trackStates.at(ftn("c")), TrackState::Selected);
}

TEST(PropertyRankingSweepIdle, NoEvictionWhenIdleTimeoutIsZero) {
  // idleTimeout=0 disables idle eviction
  RankingHarness h(5, std::chrono::milliseconds(0));
  auto sub = makeSession();

  h.ranking().registerTrack(ftn("a"), 100, {});
  h.ranking().registerTrack(ftn("b"), 90, {});

  h.ranking().addSessionToTopNGroup(2, sub, true);
  h.clearEvents();

  // Even with old activity time, should not evict when timeout is 0
  auto now = std::chrono::steady_clock::now();
  h.setActivityTime(ftn("a"), now - std::chrono::hours(1));
  h.setActivityTime(ftn("b"), now - std::chrono::hours(1));

  h.ranking().sweepIdle();

  // No promotions should occur
  EXPECT_EQ(h.selectCount(ftn("a")), 0);
  EXPECT_EQ(h.selectCount(ftn("b")), 0);
}

TEST(PropertyRankingSweepIdle, ActiveTracksNotEvicted) {
  RankingHarness h(5, std::chrono::milliseconds(100));
  auto sub = makeSession();

  h.ranking().registerTrack(ftn("a"), 100, {});
  h.ranking().registerTrack(ftn("b"), 90, {});

  h.ranking().addSessionToTopNGroup(2, sub, true);
  h.clearEvents();

  // Both tracks are active (recent activity)
  auto now = std::chrono::steady_clock::now();
  h.setActivityTime(ftn("a"), now);
  h.setActivityTime(ftn("b"), now);

  h.ranking().sweepIdle();

  // No evictions
  auto* group = h.ranking().getTopNGroup(2);
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(group->trackStates.at(ftn("a")), TrackState::Selected);
  EXPECT_EQ(group->trackStates.at(ftn("b")), TrackState::Selected);
}

} // namespace
