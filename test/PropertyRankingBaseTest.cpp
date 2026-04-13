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

  explicit RankingHarness(uint64_t maxDeselected = 5)
      : ranking_(std::make_unique<PropertyRanking>(
            kProp,
            maxDeselected,
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
// Sentinel cachedRank fix: promotion from beyond threshold
// ---------------------------------------------------------------------------
// These tests validate that tracks promoted from beyond the selection threshold
// (with cachedRank = UINT64_MAX sentinel) get their cachedRank fixed correctly.
// Bug scenario: Track at rank >= selectionThreshold has sentinel cachedRank.
// If promoted without fixing cachedRank, subsequent updateSortValue uses stale
// UINT64_MAX causing incorrect wasInTopN computation and spurious notifications.

TEST_F(PropertyRankingBaseTest, RemoveTrackFallback_FixesSentinelCachedRank) {
  // Scenario from Alan's bug report:
  // N=3, 4 tracks registered (track at rank 3 has sentinel cachedRank).
  // Remove track at rank 2 -> fallback scan promotes track from rank 3.
  // Track must have correct cachedRank=2 (new position) for updateSortValue.
  RankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(3, sub, true);

  // With N=3 and maxDeselected=5 (default), selectionThreshold = 3+5 = 8.
  // All 4 tracks get accurate cachedRank (0,1,2,3 < 8).
  // To trigger sentinel, we need tracks at rank >= threshold.
  // Use maxDeselected=0 to set threshold = N = 3.
  RankingHarness h2(/*maxDeselected=*/0);
  auto sub2 = makeSession();
  h2.ranking().addSessionToTopNGroup(3, sub2, true);

  h2.ranking().registerTrack(ftn("a"), 100, {}); // rank 0
  h2.ranking().registerTrack(ftn("b"), 80, {});  // rank 1
  h2.ranking().registerTrack(ftn("c"), 60, {});  // rank 2
  h2.ranking().registerTrack(ftn("d"), 40, {});  // rank 3 - has sentinel cachedRank

  // Verify d was selected initially (part of top-3... wait, N=3 means ranks 0,1,2)
  // Actually ranks 0,1,2 are in top-3, so d at rank 3 is NOT selected.
  h2.clearEvents();

  // Remove c (rank 2, selected) -> fallback scan promotes d (rank 3 -> rank 2)
  h2.ranking().removeTrack(ftn("c"));
  EXPECT_EQ(h2.selectCount(ftn("d"), sub2.get()), 1); // d promoted

  // Critical: subsequent updateSortValue on d should NOT cause spurious notification.
  // If cachedRank wasn't fixed, d would have UINT64_MAX and wasInTopN would be false,
  // causing duplicate selection notification.
  h2.clearEvents();

  // Move d within top-3 (from rank 2 to rank 0)
  h2.ranking().updateSortValue(ftn("d"), 110);
  // d was already selected (rank 2), now rank 0 - still selected, no new notification
  EXPECT_EQ(h2.selectCount(ftn("d"), sub2.get()), 0);
}

TEST_F(PropertyRankingBaseTest, PromoteTrackInGroup_FixesSentinelCachedRank) {
  // Test that promoteTrackInGroup (via recomputeTopNGroups slow path) also
  // fixes sentinel cachedRank when promoting a track from beyond threshold.
  //
  // Scenario: N=2, 4 tracks. Track at rank 3 has sentinel.
  // Track at rank 0 falls to rank 3 -> track at rank 2 (position N-1=1 after shift)
  // gets promoted. But we want to test rank 3 promotion.
  //
  // Better scenario: N=3, threshold=3 (maxDeselected=0), 5 tracks.
  // Track at rank 4 has sentinel. Track at rank 0 falls out -> promotes rank 3.
  RankingHarness h(/*maxDeselected=*/0);
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(3, sub, true); // threshold = 3

  h.ranking().registerTrack(ftn("a"), 100, {}); // rank 0 - selected
  h.ranking().registerTrack(ftn("b"), 80, {});  // rank 1 - selected
  h.ranking().registerTrack(ftn("c"), 60, {});  // rank 2 - selected
  h.ranking().registerTrack(ftn("d"), 40, {});  // rank 3 - sentinel cachedRank
  h.ranking().registerTrack(ftn("e"), 20, {});  // rank 4 - sentinel cachedRank
  h.clearEvents();

  // Drop "a" from rank 0 to rank 4 (value=10)
  // New order: b=80(0), c=60(1), d=40(2), e=20(3), a=10(4)
  // Promotion at position N-1=2: d gets promoted
  h.ranking().updateSortValue(ftn("a"), 10);

  // d should be selected (promoted into top-3)
  EXPECT_GE(h.selectCount(ftn("d"), sub.get()), 1);
  h.clearEvents();

  // Now update d - should NOT cause spurious notification
  // Move d within top-3 (from rank 2 to rank 0)
  h.ranking().updateSortValue(ftn("d"), 90);
  // d was already selected at rank 2, now rank 0 - no new notification
  EXPECT_EQ(h.selectCount(ftn("d"), sub.get()), 0);
}

TEST_F(PropertyRankingBaseTest, SentinelFix_SubsequentUpdateSortValueCorrect) {
  // Comprehensive test: verify that after sentinel fix, the promoted track's
  // subsequent movements are handled correctly.
  RankingHarness h(/*maxDeselected=*/0);
  auto sub2 = makeSession();
  auto sub4 = makeSession();
  h.ranking().addSessionToTopNGroup(2, sub2, true); // N=2
  h.ranking().addSessionToTopNGroup(4, sub4, true); // N=4, threshold=4

  h.ranking().registerTrack(ftn("a"), 100, {}); // rank 0
  h.ranking().registerTrack(ftn("b"), 80, {});  // rank 1
  h.ranking().registerTrack(ftn("c"), 60, {});  // rank 2
  h.ranking().registerTrack(ftn("d"), 40, {});  // rank 3
  h.ranking().registerTrack(ftn("e"), 20, {});  // rank 4 - sentinel

  // Initial state: sub2 gets a,b; sub4 gets a,b,c,d
  h.clearEvents();

  // Remove "c" (rank 2, selected for sub4 only)
  // After removal: a=100(0), b=80(1), d=40(2), e=20(3)
  // For sub4 (N=4): d shifts to rank 2 (still selected), e shifts to rank 3 (promoted)
  // e had sentinel cachedRank, now should have cachedRank=3
  h.ranking().removeTrack(ftn("c"));

  // e should be promoted into sub4's top-4
  EXPECT_GE(h.selectCount(ftn("e"), sub4.get()), 1);
  h.clearEvents();

  // Now move e from rank 3 to rank 1 (entering sub2's top-2)
  h.ranking().updateSortValue(ftn("e"), 90);
  // Order: a=100(0), e=90(1), b=80(2), d=40(3)

  // e enters sub2 (was rank 3, now rank 1)
  EXPECT_GE(h.selectCount(ftn("e"), sub2.get()), 1);
  // e should NOT get duplicate notification for sub4 (was already selected at rank 3)
  EXPECT_EQ(h.selectCount(ftn("e"), sub4.get()), 0);
}

// ---------------------------------------------------------------------------
// Algorithm correctness: Step 4 cachedRank update verification
// ---------------------------------------------------------------------------
// These tests verify that cachedRank for OTHER tracks in the affected range
// are updated correctly after updateSortValue Step 4.

TEST_F(PropertyRankingBaseTest, UpdateSortValue_AffectedRanksCachedCorrectly) {
  // Scenario: track "a" moves from rank 0 to rank 3.
  // Tracks b, c, d should all shift up by 1, and their cachedRanks must be correct.
  // Then update track "b" (which was in the affected range) - should work correctly.
  RankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(2, sub, true);

  // Initial: a=100(0), b=80(1), c=60(2), d=40(3), e=20(4)
  h.ranking().registerTrack(ftn("a"), 100, {});
  h.ranking().registerTrack(ftn("b"), 80, {});
  h.ranking().registerTrack(ftn("c"), 60, {});
  h.ranking().registerTrack(ftn("d"), 40, {});
  h.ranking().registerTrack(ftn("e"), 20, {});
  h.clearEvents();

  // Drop "a" from rank 0 to rank 3 (value=35)
  // New order: b=80(0), c=60(1), d=40(2), a=35(3), e=20(4)
  h.ranking().updateSortValue(ftn("a"), 35);

  // b should be selected (it's now rank 0, was rank 1)
  // c enters top-2 (was rank 2, now rank 1)
  EXPECT_GE(h.selectCount(ftn("c"), sub.get()), 1);
  h.clearEvents();

  // Now update "b" (was in affected range 0-3) to verify its cachedRank is correct
  // Move b from rank 0 to rank 2 (value=45)
  // New order: c=60(0), d=40(1), b=45(2)... wait, 45 < 60 and 45 > 40, so:
  // c=60(0), b=45(1), d=40(2), a=35(3), e=20(4)
  h.ranking().updateSortValue(ftn("b"), 45);

  // b stays in top-2 (rank 0 -> rank 1), no new notification needed
  // If b's cachedRank was wrong (e.g., still 1 from before), this would cause issues
  EXPECT_EQ(h.selectCount(ftn("b"), sub.get()), 0);

  // d should NOT be selected (it's at rank 2, outside top-2)
  EXPECT_EQ(h.selectCount(ftn("d"), sub.get()), 0);
}

TEST_F(PropertyRankingBaseTest, UpdateSortValue_SequentialMovesOnSameTrack) {
  // Verify cachedRank stays correct across multiple sequential updateSortValue calls
  // on the same track.
  RankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(2, sub, true);

  h.ranking().registerTrack(ftn("a"), 100, {});
  h.ranking().registerTrack(ftn("b"), 80, {});
  h.ranking().registerTrack(ftn("c"), 60, {});
  h.ranking().registerTrack(ftn("d"), 40, {});
  h.clearEvents();

  // Move "d" up: 40 -> 90 (rank 3 -> rank 1, enters top-2)
  h.ranking().updateSortValue(ftn("d"), 90);
  EXPECT_EQ(h.selectCount(ftn("d"), sub.get()), 1);
  h.clearEvents();

  // Move "d" down: 90 -> 50 (rank 1 -> rank 3, exits top-2)
  // Order: a=100(0), b=80(1), c=60(2), d=50(3)
  h.ranking().updateSortValue(ftn("d"), 50);
  // d exits top-2, no selection notification (deselection not tested here)
  EXPECT_EQ(h.selectCount(ftn("d"), sub.get()), 0);
  h.clearEvents();

  // Move "d" back up: 50 -> 85 (rank 3 -> rank 1, enters top-2 again)
  h.ranking().updateSortValue(ftn("d"), 85);
  EXPECT_EQ(h.selectCount(ftn("d"), sub.get()), 1);
  h.clearEvents();

  // Move "d" within top-2: 85 -> 110 (rank 1 -> rank 0)
  // If cachedRank is wrong, this could cause spurious notification
  h.ranking().updateSortValue(ftn("d"), 110);
  // d was already selected, stays selected - no new notification
  EXPECT_EQ(h.selectCount(ftn("d"), sub.get()), 0);
}

TEST_F(PropertyRankingBaseTest, UpdateSortValue_ExactBoundaryPromotion) {
  // Track moves from rank N to rank N-1 (exactly crosses into top-N)
  RankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(3, sub, true);

  h.ranking().registerTrack(ftn("a"), 100, {}); // rank 0
  h.ranking().registerTrack(ftn("b"), 80, {});  // rank 1
  h.ranking().registerTrack(ftn("c"), 60, {});  // rank 2 - last in top-3
  h.ranking().registerTrack(ftn("d"), 40, {});  // rank 3 - first outside top-3
  h.clearEvents();

  // Move "d" from rank 3 to rank 2 (exactly N-1), entering top-3
  // d=65 > c=60, so order: a=100(0), b=80(1), d=65(2), c=60(3)
  h.ranking().updateSortValue(ftn("d"), 65);

  // d should be selected (entered top-3)
  EXPECT_EQ(h.selectCount(ftn("d"), sub.get()), 1);
  // c should NOT get duplicate notification (it was demoted, not selected)
  EXPECT_EQ(h.selectCount(ftn("c"), sub.get()), 0);
}

TEST_F(PropertyRankingBaseTest, UpdateSortValue_ExactBoundaryDemotion) {
  // Track moves from rank N-1 to rank N (exactly falls out of top-N)
  RankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(3, sub, true);

  h.ranking().registerTrack(ftn("a"), 100, {}); // rank 0
  h.ranking().registerTrack(ftn("b"), 80, {});  // rank 1
  h.ranking().registerTrack(ftn("c"), 60, {});  // rank 2 - last in top-3
  h.ranking().registerTrack(ftn("d"), 40, {});  // rank 3 - first outside top-3
  h.clearEvents();

  // Move "c" from rank 2 to rank 3 (exactly N), exiting top-3
  // c=35 < d=40, so order: a=100(0), b=80(1), d=40(2), c=35(3)
  h.ranking().updateSortValue(ftn("c"), 35);

  // d should be promoted (entered top-3)
  EXPECT_EQ(h.selectCount(ftn("d"), sub.get()), 1);
  // c exited, no select notification
  EXPECT_EQ(h.selectCount(ftn("c"), sub.get()), 0);
}

TEST_F(PropertyRankingBaseTest, UpdateSortValue_FallOutOfAllGroups) {
  // Track at rank 0 falls out of ALL groups simultaneously
  RankingHarness h;
  auto sub2 = makeSession();
  auto sub3 = makeSession();
  h.ranking().addSessionToTopNGroup(2, sub2, true);
  h.ranking().addSessionToTopNGroup(3, sub3, true);

  h.ranking().registerTrack(ftn("a"), 100, {}); // rank 0 - in both groups
  h.ranking().registerTrack(ftn("b"), 80, {});  // rank 1 - in both groups
  h.ranking().registerTrack(ftn("c"), 60, {});  // rank 2 - in sub3 only
  h.ranking().registerTrack(ftn("d"), 40, {});  // rank 3 - outside both
  h.ranking().registerTrack(ftn("e"), 20, {});  // rank 4 - outside both
  h.clearEvents();

  // Drop "a" from rank 0 to rank 4 (value=10)
  // New order: b=80(0), c=60(1), d=40(2), e=20(3), a=10(4)
  h.ranking().updateSortValue(ftn("a"), 10);

  // For sub2 (top-2): c promoted (was rank 2, now rank 1)
  EXPECT_EQ(h.selectCount(ftn("c"), sub2.get()), 1);
  // For sub3 (top-3): d promoted (was rank 3, now rank 2)
  EXPECT_EQ(h.selectCount(ftn("d"), sub3.get()), 1);
  // a should NOT be selected for either group
  EXPECT_EQ(h.selectCount(ftn("a"), sub2.get()), 0);
  EXPECT_EQ(h.selectCount(ftn("a"), sub3.get()), 0);
}

TEST_F(PropertyRankingBaseTest, RegisterTrack_ShiftedTracksHaveCorrectRanks) {
  // Verify that registerTrack correctly increments cachedRank for shifted tracks,
  // and subsequent updateSortValue on those tracks works correctly.
  RankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(2, sub, true);

  // Register tracks in order
  h.ranking().registerTrack(ftn("a"), 100, {}); // rank 0
  h.ranking().registerTrack(ftn("b"), 80, {});  // rank 1
  h.ranking().registerTrack(ftn("c"), 60, {});  // rank 2
  h.clearEvents();

  // Register a track that inserts at rank 1, shifting b and c down
  h.ranking().registerTrack(ftn("d"), 90, {}); // d=90 goes to rank 1
  // New order: a=100(0), d=90(1), b=80(2), c=60(3)

  // d should be selected (rank 1, in top-2)
  EXPECT_EQ(h.selectCount(ftn("d"), sub.get()), 1);
  h.clearEvents();

  // b was shifted from rank 1 to rank 2. Verify its cachedRank is correct
  // by doing an updateSortValue that should cross threshold.
  // Move b from rank 2 to rank 0
  h.ranking().updateSortValue(ftn("b"), 110);
  // If b's cachedRank was still 1 (not updated to 2), this would be:
  // oldRank=1, newRank=0, which doesn't cross threshold -> no notification (BUG)
  // With correct cachedRank=2: oldRank=2, newRank=0, crosses threshold -> notification (CORRECT)
  EXPECT_EQ(h.selectCount(ftn("b"), sub.get()), 1);
}

TEST_F(PropertyRankingBaseTest, RemoveTrack_ShiftedTracksHaveCorrectRanks) {
  // Verify that removeTrack correctly decrements cachedRank for shifted tracks,
  // and subsequent updateSortValue on those tracks works correctly.
  RankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(2, sub, true);

  h.ranking().registerTrack(ftn("a"), 100, {}); // rank 0
  h.ranking().registerTrack(ftn("b"), 80, {});  // rank 1
  h.ranking().registerTrack(ftn("c"), 60, {});  // rank 2
  h.ranking().registerTrack(ftn("d"), 40, {});  // rank 3
  h.clearEvents();

  // Remove "a" at rank 0 - all subsequent tracks shift up
  // New order: b=80(0), c=60(1), d=40(2)
  h.ranking().removeTrack(ftn("a"));

  // c should be promoted (rank 2 -> rank 1, enters top-2)
  EXPECT_EQ(h.selectCount(ftn("c"), sub.get()), 1);
  h.clearEvents();

  // d was shifted from rank 3 to rank 2. Verify its cachedRank is correct.
  // Move d from rank 2 to rank 0
  h.ranking().updateSortValue(ftn("d"), 110);
  // If d's cachedRank was still 3 (not updated to 2), this would compute
  // wasInTopN incorrectly -> possibly spurious notification
  // With correct cachedRank=2: oldRank=2, newRank=0, crosses N=2 threshold
  EXPECT_EQ(h.selectCount(ftn("d"), sub.get()), 1);
}

TEST_F(PropertyRankingBaseTest, PromotionFromDeselectedQueue_CorrectCachedRank) {
  // Verify that when a track is promoted from the deselected queue,
  // its cachedRank (which may be stale) doesn't cause issues.
  RankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(2, sub, true);

  h.ranking().registerTrack(ftn("a"), 100, {}); // rank 0, selected
  h.ranking().registerTrack(ftn("b"), 80, {});  // rank 1, selected
  h.ranking().registerTrack(ftn("c"), 60, {});  // rank 2
  h.clearEvents();

  // "d" registers at rank 0, pushing a to rank 1, b to rank 2 (enters deselected queue)
  h.ranking().registerTrack(ftn("d"), 110, {});
  // Order: d=110(0), a=100(1), b=80(2), c=60(3)
  h.clearEvents();

  // Remove "d" - b should be promoted from deselected queue
  h.ranking().removeTrack(ftn("d"));
  // Order: a=100(0), b=80(1), c=60(2)
  EXPECT_EQ(h.selectCount(ftn("b"), sub.get()), 1);
  h.clearEvents();

  // Now update b - should NOT cause spurious notification
  // Move b within top-2 (rank 1 -> rank 0)
  h.ranking().updateSortValue(ftn("b"), 105);
  // b was already selected, stays selected
  EXPECT_EQ(h.selectCount(ftn("b"), sub.get()), 0);
}

// ---------------------------------------------------------------------------
// Edge cases: Empty ranking, single track, tiebreaker scenarios
// ---------------------------------------------------------------------------

TEST_F(PropertyRankingBaseTest, EdgeCase_EmptyRanking_NoErrors) {
  // Operations on empty ranking should not crash
  RankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(3, sub, true);

  // No tracks registered - verify no events and no crashes
  EXPECT_EQ(h.selected_.size(), 0u);

  // Update non-existent track should silently fail
  h.ranking().updateSortValue(ftn("nonexistent"), 100);
  EXPECT_EQ(h.selected_.size(), 0u);

  // Remove non-existent track should silently fail
  h.ranking().removeTrack(ftn("nonexistent"));
  EXPECT_EQ(h.selected_.size(), 0u);
}

TEST_F(PropertyRankingBaseTest, EdgeCase_SingleTrack) {
  // Single track operations
  RankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(1, sub, true);

  h.ranking().registerTrack(ftn("a"), 50, {});
  EXPECT_EQ(h.selectCount(ftn("a"), sub.get()), 1);
  h.clearEvents();

  // Update single track - stays in top-1
  h.ranking().updateSortValue(ftn("a"), 100);
  EXPECT_EQ(h.selected_.size(), 0u); // no change notification

  // Remove single track
  h.ranking().removeTrack(ftn("a"));
  // No promotion possible (no other tracks)
  EXPECT_EQ(h.selected_.size(), 0u);
}

TEST_F(PropertyRankingBaseTest, EdgeCase_SameValueTiebreaker) {
  // Tracks with same value use arrival sequence (tiebreaker)
  RankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(2, sub, true);

  // All tracks have same value - should be ordered by arrival
  h.ranking().registerTrack(ftn("a"), 50, {}); // arrives first, rank 0
  h.ranking().registerTrack(ftn("b"), 50, {}); // arrives second, rank 1
  h.ranking().registerTrack(ftn("c"), 50, {}); // arrives third, rank 2

  // a and b should be selected (ranks 0,1)
  EXPECT_EQ(h.selectCount(ftn("a"), sub.get()), 1);
  EXPECT_EQ(h.selectCount(ftn("b"), sub.get()), 1);
  EXPECT_EQ(h.selectCount(ftn("c"), sub.get()), 0);
  h.clearEvents();

  // Update "c" to same value - should stay at rank 2 (tiebreaker preserved)
  h.ranking().updateSortValue(ftn("c"), 50);
  EXPECT_EQ(h.selected_.size(), 0u); // no change
}

TEST_F(PropertyRankingBaseTest, EdgeCase_RemoveOnlySelectedTrack) {
  // Remove the only selected track when there are other tracks to promote
  RankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(1, sub, true);

  h.ranking().registerTrack(ftn("a"), 100, {}); // selected
  h.ranking().registerTrack(ftn("b"), 80, {});  // rank 1, not selected
  h.clearEvents();

  h.ranking().removeTrack(ftn("a"));
  EXPECT_EQ(h.selectCount(ftn("b"), sub.get()), 1); // b promoted
}

TEST_F(PropertyRankingBaseTest, EdgeCase_MultipleRemoves) {
  // Sequential removes - verify ranks stay correct throughout
  RankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(2, sub, true);

  h.ranking().registerTrack(ftn("a"), 100, {}); // rank 0
  h.ranking().registerTrack(ftn("b"), 80, {});  // rank 1
  h.ranking().registerTrack(ftn("c"), 60, {});  // rank 2
  h.ranking().registerTrack(ftn("d"), 40, {});  // rank 3
  h.ranking().registerTrack(ftn("e"), 20, {});  // rank 4
  h.clearEvents();

  // Remove a: b=80(0), c=60(1), d=40(2), e=20(3)
  h.ranking().removeTrack(ftn("a"));
  EXPECT_EQ(h.selectCount(ftn("c"), sub.get()), 1); // c promoted
  h.clearEvents();

  // Remove b: c=60(0), d=40(1), e=20(2)
  h.ranking().removeTrack(ftn("b"));
  EXPECT_EQ(h.selectCount(ftn("d"), sub.get()), 1); // d promoted
  h.clearEvents();

  // Remove c: d=40(0), e=20(1)
  h.ranking().removeTrack(ftn("c"));
  EXPECT_EQ(h.selectCount(ftn("e"), sub.get()), 1); // e promoted
  h.clearEvents();

  // Verify d is still rank 0 by moving it
  h.ranking().updateSortValue(ftn("d"), 50);
  // d stays at rank 0 (50 > 20), no notification
  EXPECT_EQ(h.selectCount(ftn("d"), sub.get()), 0);
}

TEST_F(PropertyRankingBaseTest, EdgeCase_RegisterAtAllPositions) {
  // Register tracks at various positions to verify rank insertion logic
  RankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(3, sub, true);

  // Register in non-sequential order
  h.ranking().registerTrack(ftn("mid"), 50, {});   // first track, rank 0
  h.ranking().registerTrack(ftn("high"), 100, {}); // inserts at rank 0, mid shifts to 1
  h.ranking().registerTrack(ftn("low"), 20, {});   // inserts at rank 2
  h.ranking().registerTrack(ftn("highest"), 150, {}); // inserts at rank 0, shifts all

  // Order: highest=150(0), high=100(1), mid=50(2), low=20(3)
  // Top-3: highest, high, mid
  // Note: "low" was selected when registered at rank 2, then demoted when "highest" arrived
  EXPECT_EQ(h.selectCount(ftn("highest"), sub.get()), 1);
  EXPECT_EQ(h.selectCount(ftn("high"), sub.get()), 1);
  EXPECT_EQ(h.selectCount(ftn("mid"), sub.get()), 1);
  EXPECT_EQ(h.selectCount(ftn("low"), sub.get()), 1); // was selected at rank 2, then demoted
  h.clearEvents();

  // Verify ranks by updating "low" to cross threshold (re-enter top-3)
  h.ranking().updateSortValue(ftn("low"), 75);
  // Order: highest=150(0), high=100(1), low=75(2), mid=50(3)
  EXPECT_EQ(h.selectCount(ftn("low"), sub.get()), 1); // low re-promoted
}

TEST_F(PropertyRankingBaseTest, EdgeCase_LargeRankJump) {
  // Track jumps many positions at once
  RankingHarness h(/*maxDeselected=*/0); // threshold = N
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(3, sub, true); // threshold = 3

  // Register 10 tracks
  for (int i = 0; i < 10; i++) {
    h.ranking().registerTrack(ftn(std::to_string(i)), 100 - i * 10, {});
  }
  // Order: 0=100(0), 1=90(1), 2=80(2), 3=70(3), 4=60(4), ...
  h.clearEvents();

  // Move track "9" (rank 9, value 10) to rank 0 (value 200)
  h.ranking().updateSortValue(ftn("9"), 200);
  // Order: 9=200(0), 0=100(1), 1=90(2), 2=80(3), ...
  EXPECT_EQ(h.selectCount(ftn("9"), sub.get()), 1); // 9 enters top-3
  h.clearEvents();

  // Move track "9" back to rank 9 (value 5)
  h.ranking().updateSortValue(ftn("9"), 5);
  // Order: 0=100(0), 1=90(1), 2=80(2), ... 9=5(9)
  EXPECT_EQ(h.selectCount(ftn("2"), sub.get()), 1); // 2 re-enters top-3
}

TEST_F(PropertyRankingBaseTest, EdgeCase_UpdateToExactlyPreviousValue) {
  // Update track to a value that places it exactly where another track was
  RankingHarness h;
  auto sub = makeSession();
  h.ranking().addSessionToTopNGroup(2, sub, true);

  h.ranking().registerTrack(ftn("a"), 100, {}); // rank 0
  h.ranking().registerTrack(ftn("b"), 80, {});  // rank 1
  h.ranking().registerTrack(ftn("c"), 60, {});  // rank 2
  h.clearEvents();

  // Move c to exactly b's value
  h.ranking().updateSortValue(ftn("c"), 80);
  // c has same value as b but arrived later, so c stays below b
  // Order: a=100(0), b=80(1), c=80(2)
  // c is still at rank 2, no threshold crossing
  EXPECT_EQ(h.selected_.size(), 0u);

  // Move c to just above b
  h.ranking().updateSortValue(ftn("c"), 81);
  // Order: a=100(0), c=81(1), b=80(2)
  // c enters top-2
  EXPECT_EQ(h.selectCount(ftn("c"), sub.get()), 1);
}

} // namespace
