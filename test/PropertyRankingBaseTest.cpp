/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * Unit tests for PropertyRanking top-N base logic (no self-exclusion).
 *
 * Covers:
 * - registerTrack + addSubscriberToTopNGroup → correct sessions notified at top-N boundary
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
#include <moxygen/MoQSession.h>
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
    SessionId session;
    bool forward;
    const moxygen::Subscriber* subscriber;
  };
  struct EvictEvent {
    FullTrackName ftn;
    SessionId session;
  };

  explicit RankingHarness(
      uint64_t maxDeselected = 5,
      std::chrono::milliseconds idleTimeout = std::chrono::milliseconds(0),
      std::chrono::milliseconds sweepThrottle = std::chrono::milliseconds(0)
  )
      : ranking_(std::make_unique<PropertyRanking>(
            kProp,
            maxDeselected,
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
                const std::vector<std::pair<Participant, bool>>& subscribers
            ) {
              for (auto& [s, fwd] : subscribers) {
                selected_.push_back({f, s.info->sessionId, fwd, s.subscriber.get()});
              }
            },
            [this](const FullTrackName& f, const Participant& s, bool fwd) {
              selected_.push_back({f, s.info->sessionId, fwd, s.subscriber.get()});
            },
            [this](const FullTrackName& f, const Participant& s) {
              evicted_.push_back({f, s.info->sessionId});
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
  int selectCount(const FullTrackName& f, SessionId s) const {
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

class PropertyRankingBaseTest : public ::testing::Test {
protected:
  // PropertyRanking keys on the participant's ID and passes it back through callbacks.
  static Participant makeParticipant() {
    return Participant(
        std::make_shared<const ParticipantInfo>(
            ParticipantInfo{.sessionId = MoQSession::makeSessionId()}
        ),
        nullptr,
        nullptr
    );
  }

  static SessionId idOf(const Participant& participant) { return participant.info->sessionId; }

  // Placeholder publisher for tracks whose publisher identity doesn't matter.
  Participant defaultPublisher_{makeParticipant()};
};

// ---------------------------------------------------------------------------
// registerTrack + addSubscriberToTopNGroup
// ---------------------------------------------------------------------------

TEST_F(PropertyRankingBaseTest, RegisterThenSubscribe_TopNBoundary) {
  RankingHarness h;

  // 3 tracks registered before any subscriber
  h.ranking().registerTrack(ftn("a"), 90, idOf(defaultPublisher_));
  h.ranking().registerTrack(ftn("b"), 80, idOf(defaultPublisher_));
  h.ranking().registerTrack(ftn("c"), 70, idOf(defaultPublisher_));
  h.ranking().registerTrack(ftn("d"), 20, idOf(defaultPublisher_));

  // Subscriber joins wanting top-2
  auto sub = makeParticipant();
  h.ranking().addSubscriberToTopNGroup(2, sub, /*forward=*/true);

  // Should have received exactly ftn("a") and ftn("b")
  EXPECT_EQ(h.selectCount(ftn("a"), idOf(sub)), 1);
  EXPECT_EQ(h.selectCount(ftn("b"), idOf(sub)), 1);
  EXPECT_EQ(h.selectCount(ftn("c"), idOf(sub)), 0);
  EXPECT_EQ(h.selectCount(ftn("d"), idOf(sub)), 0);
}

TEST_F(PropertyRankingBaseTest, SelectPassesTheStoredSubscriber) {
  RankingHarness h;
  h.ranking().registerTrack(ftn("a"), 100, idOf(defaultPublisher_));
  auto subscriber = std::make_shared<moxygen::Subscriber>();
  Participant sub(makeParticipant().info, nullptr, subscriber);
  h.ranking().addSubscriberToTopNGroup(1, sub, /*forward=*/true);

  ASSERT_EQ(h.selected_.size(), 1);
  EXPECT_EQ(h.selected_[0].subscriber, subscriber.get());
}

TEST_F(PropertyRankingBaseTest, SubscribeThenRegister_TopNBoundary) {
  RankingHarness h;

  auto sub = makeParticipant();
  h.ranking().addSubscriberToTopNGroup(2, sub, /*forward=*/true);

  // Tracks arrive after subscriber
  h.ranking().registerTrack(ftn("a"), 90, idOf(defaultPublisher_));
  h.ranking().registerTrack(ftn("b"), 80, idOf(defaultPublisher_));
  h.ranking().registerTrack(ftn("c"), 70, idOf(defaultPublisher_)); // rank 2 — not in top-2

  EXPECT_EQ(h.selectCount(ftn("a"), idOf(sub)), 1);
  EXPECT_EQ(h.selectCount(ftn("b"), idOf(sub)), 1);
  EXPECT_EQ(h.selectCount(ftn("c"), idOf(sub)), 0);
}

TEST_F(PropertyRankingBaseTest, ForwardFlagPassedThrough_False) {
  RankingHarness h;
  auto sub = makeParticipant();
  h.ranking().addSubscriberToTopNGroup(1, sub, /*forward=*/false);
  h.ranking().registerTrack(ftn("a"), 10, idOf(defaultPublisher_));

  ASSERT_EQ(h.selected_.size(), 1u);
  EXPECT_FALSE(h.selected_[0].forward);
}

TEST_F(PropertyRankingBaseTest, ForwardFlagPassedThrough_True) {
  RankingHarness h;
  auto sub = makeParticipant();
  h.ranking().addSubscriberToTopNGroup(1, sub, /*forward=*/true);
  h.ranking().registerTrack(ftn("a"), 10, idOf(defaultPublisher_));

  ASSERT_EQ(h.selected_.size(), 1u);
  EXPECT_TRUE(h.selected_[0].forward);
}

// ---------------------------------------------------------------------------
// updateSortValue fast path
// ---------------------------------------------------------------------------

TEST_F(PropertyRankingBaseTest, FastPath_NoNotificationWhenThresholdNotCrossed) {
  RankingHarness h;
  auto sub = makeParticipant();
  h.ranking().addSubscriberToTopNGroup(2, sub, true);

  // 4 tracks: top-2 = {a=90, b=80}; {c=70, d=50} are outside
  h.ranking().registerTrack(ftn("a"), 90, idOf(defaultPublisher_));
  h.ranking().registerTrack(ftn("b"), 80, idOf(defaultPublisher_));
  h.ranking().registerTrack(ftn("c"), 70, idOf(defaultPublisher_)); // rank 2 — just outside top-2
  h.ranking().registerTrack(ftn("d"), 50, idOf(defaultPublisher_)); // rank 3
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
  auto sub = makeParticipant();
  h.ranking().addSubscriberToTopNGroup(1, sub, true);
  h.ranking().registerTrack(ftn("a"), 50, idOf(defaultPublisher_));
  h.clearEvents();

  h.ranking().updateSortValue(ftn("a"), 50); // no change
  EXPECT_EQ(h.selected_.size(), 0u);
}

// ---------------------------------------------------------------------------
// updateSortValue slow path — promotions and evictions
// ---------------------------------------------------------------------------

TEST_F(PropertyRankingBaseTest, SlowPath_TrackPromotedWhenCrossesThreshold) {
  RankingHarness h;
  auto sub = makeParticipant();
  h.ranking().addSubscriberToTopNGroup(1, sub, true);

  h.ranking().registerTrack(ftn("a"), 80, idOf(defaultPublisher_));
  h.ranking().registerTrack(ftn("b"), 60, idOf(defaultPublisher_)); // rank 1 — not in top-1
  h.clearEvents();

  // "b" jumps to rank 0 — should be selected, "a" deselected
  h.ranking().updateSortValue(ftn("b"), 90);
  EXPECT_EQ(h.selectCount(ftn("b"), idOf(sub)), 1);
  EXPECT_EQ(h.selectCount(ftn("a"), idOf(sub)), 0);
}

TEST_F(PropertyRankingBaseTest, SlowPath_MultipleTopNGroups) {
  RankingHarness h;
  auto sub1 = makeParticipant(); // wants top-1
  auto sub3 = makeParticipant(); // wants top-3

  h.ranking().addSubscriberToTopNGroup(1, sub1, true);
  h.ranking().addSubscriberToTopNGroup(3, sub3, true);

  h.ranking().registerTrack(ftn("a"), 100, idOf(defaultPublisher_));
  h.ranking().registerTrack(ftn("b"), 80, idOf(defaultPublisher_));
  h.ranking().registerTrack(ftn("c"), 60, idOf(defaultPublisher_));
  h.ranking()
      .registerTrack(ftn("d"), 40, idOf(defaultPublisher_)); // rank 3 — NOT in top-3, not in top-1
  h.clearEvents();

  // "d" surpasses "a" — crosses threshold for both groups
  h.ranking().updateSortValue(ftn("d"), 110);

  // sub1 (top-1) should now receive "d"
  EXPECT_GE(h.selectCount(ftn("d"), idOf(sub1)), 1);
  // sub3 (top-3) should also receive "d" — it was NOT in top-3 before (rank 3 is not < 3),
  // now it's rank 0 so sub3 should be notified
  EXPECT_GE(h.selectCount(ftn("d"), idOf(sub3)), 1);
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
  auto sub = makeParticipant();
  h.ranking().addSubscriberToTopNGroup(1, sub, true);

  h.ranking().registerTrack(ftn("a"), 90, idOf(defaultPublisher_)); // selected
  h.ranking().registerTrack(ftn("b"), 80, idOf(defaultPublisher_)); // rank 1, unselected
  h.clearEvents();

  // c arrives louder than a → a falls to deselected queue
  h.ranking().registerTrack(
      ftn("c"),
      95,
      idOf(defaultPublisher_)
  ); // c→rank0(selected), a→rank1(deselected queue)
  h.clearEvents();

  // Remove c (selected): a should be promoted from deselected queue
  h.ranking().removeTrack(ftn("c"));
  EXPECT_EQ(h.selectCount(ftn("a"), idOf(sub)), 1);
  EXPECT_EQ(h.selectCount(ftn("b"), idOf(sub)), 0);
}

TEST_F(PropertyRankingBaseTest, RemoveSelected_PromotesFromRankedList) {
  RankingHarness h;
  auto sub = makeParticipant();
  h.ranking().addSubscriberToTopNGroup(1, sub, true);

  h.ranking().registerTrack(ftn("a"), 90, idOf(defaultPublisher_));
  h.ranking().registerTrack(ftn("b"), 70, idOf(defaultPublisher_)); // rank 1
  h.clearEvents();

  h.ranking().removeTrack(ftn("a"));
  EXPECT_EQ(h.selectCount(ftn("b"), idOf(sub)), 1);
}

TEST_F(PropertyRankingBaseTest, RemoveDeselected_NoPromotionNeeded) {
  RankingHarness h;
  auto sub = makeParticipant();
  h.ranking().addSubscriberToTopNGroup(1, sub, true);

  h.ranking().registerTrack(ftn("a"), 90, idOf(defaultPublisher_));
  h.ranking().registerTrack(ftn("b"), 70, idOf(defaultPublisher_)); // rank 1 — deselected
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
  auto sub1 = makeParticipant(); // top-1
  auto sub3 = makeParticipant(); // top-3

  h.ranking().addSubscriberToTopNGroup(1, sub1, true);
  h.ranking().addSubscriberToTopNGroup(3, sub3, true);

  h.ranking().registerTrack(ftn("a"), 100, idOf(defaultPublisher_));
  h.ranking().registerTrack(ftn("b"), 80, idOf(defaultPublisher_));
  h.ranking().registerTrack(ftn("c"), 60, idOf(defaultPublisher_));
  h.ranking().registerTrack(ftn("d"), 40, idOf(defaultPublisher_));

  // sub1 should have received only "a"
  EXPECT_EQ(h.selectCount(ftn("a"), idOf(sub1)), 1);
  EXPECT_EQ(h.selectCount(ftn("b"), idOf(sub1)), 0);

  // sub3 should have received "a", "b", "c"
  EXPECT_EQ(h.selectCount(ftn("a"), idOf(sub3)), 1);
  EXPECT_EQ(h.selectCount(ftn("b"), idOf(sub3)), 1);
  EXPECT_EQ(h.selectCount(ftn("c"), idOf(sub3)), 1);
  EXPECT_EQ(h.selectCount(ftn("d"), idOf(sub3)), 0);
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
  auto sub = makeParticipant();
  h.ranking().addSubscriberToTopNGroup(1, sub, true);

  h.ranking().registerTrack(ftn("a"), 90, idOf(defaultPublisher_)); // selected
  h.ranking().registerTrack(
      ftn("b"),
      95,
      idOf(defaultPublisher_)
  ); // b→selected, a→deselected queue (size 1)
  h.ranking().registerTrack(
      ftn("c"),
      100,
      idOf(defaultPublisher_)
  );                                // c→selected, b→deselected queue (size 2)
  EXPECT_EQ(h.evicted_.size(), 0u); // not yet

  h.ranking().registerTrack(
      ftn("d"),
      110,
      idOf(defaultPublisher_)
  );                                // d→selected, c→deselected queue (size 3 > 2)
  EXPECT_GE(h.evicted_.size(), 1u); // oldest entry evicted
}

// ---------------------------------------------------------------------------
// Session removal
// ---------------------------------------------------------------------------

TEST_F(PropertyRankingBaseTest, RemoveSession_GroupRemainsIfOtherSessionsExist) {
  RankingHarness h;
  auto sub1 = makeParticipant();
  auto sub2 = makeParticipant();

  h.ranking().addSubscriberToTopNGroup(2, sub1, true);
  h.ranking().addSubscriberToTopNGroup(2, sub2, true);

  h.ranking().removeSubscriberFromTopNGroup(2, idOf(sub1));

  const auto* grp = h.ranking().getTopNGroup(2);
  ASSERT_NE(grp, nullptr);
  EXPECT_EQ(grp->sessions.size(), 1u);
}

TEST_F(PropertyRankingBaseTest, RemoveLastSession_GroupRemoved) {
  RankingHarness h;
  auto sub = makeParticipant();
  h.ranking().addSubscriberToTopNGroup(3, sub, true);

  h.ranking().removeSubscriberFromTopNGroup(3, idOf(sub));

  EXPECT_EQ(h.ranking().getTopNGroup(3), nullptr);
  EXPECT_TRUE(h.ranking().empty());
}

TEST_F(PropertyRankingBaseTest, RemoveLastSession_ThresholdUpdated) {
  RankingHarness h;
  auto sub1 = makeParticipant();
  auto sub2 = makeParticipant();

  h.ranking().addSubscriberToTopNGroup(1, sub1, true);
  h.ranking().addSubscriberToTopNGroup(3, sub2, true);

  h.ranking().removeSubscriberFromTopNGroup(3, idOf(sub2)); // group N=3 gone

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
  auto sub2 = makeParticipant(); // top-2
  auto sub4 = makeParticipant(); // top-4

  h.ranking().addSubscriberToTopNGroup(2, sub2, true);
  h.ranking().addSubscriberToTopNGroup(4, sub4, true);

  h.ranking().registerTrack(ftn("a"), 100, idOf(defaultPublisher_)); // rank 0
  h.ranking().registerTrack(ftn("b"), 80, idOf(defaultPublisher_));  // rank 1
  h.ranking().registerTrack(ftn("c"), 60, idOf(defaultPublisher_));  // rank 2
  h.ranking().registerTrack(ftn("d"), 40, idOf(defaultPublisher_));  // rank 3 — in top-4, not top-2
  h.clearEvents();

  // d jumps to rank 1: crosses the N=2 boundary (3→1), stays inside N=4
  h.ranking().updateSortValue(ftn("d"), 90); // order: a=100, d=90, b=80, c=60

  // sub2 should be notified about d entering top-2
  EXPECT_EQ(h.selectCount(ftn("d"), idOf(sub2)), 1);
  // sub4 should NOT be notified — d was already in its top-4
  EXPECT_EQ(h.selectCount(ftn("d"), idOf(sub4)), 0);
  // b was at rank 1 (selected for both), falls to rank 2: leaves top-2
  // sub2 gets no additional select events (b was demoted, not selected)
  EXPECT_EQ(h.selectCount(ftn("b"), idOf(sub2)), 0);
}

// ---------------------------------------------------------------------------
// removeTrack fallback: ranked list scan promotes highest non-selected track
// ---------------------------------------------------------------------------

TEST_F(PropertyRankingBaseTest, RemoveSelected_FallbackPicksHighestNonSelected) {
  // top-2, tracks a=100(sel), b=80(sel), c=60, d=40
  // Remove a: fallback scan should promote c (rank 2, highest non-selected),
  // not d (rank 3).
  RankingHarness h;
  auto sub = makeParticipant();
  h.ranking().addSubscriberToTopNGroup(2, sub, true);

  h.ranking().registerTrack(ftn("a"), 100, idOf(defaultPublisher_));
  h.ranking().registerTrack(ftn("b"), 80, idOf(defaultPublisher_));
  h.ranking().registerTrack(ftn("c"), 60, idOf(defaultPublisher_));
  h.ranking().registerTrack(ftn("d"), 40, idOf(defaultPublisher_));
  h.clearEvents();

  h.ranking().removeTrack(ftn("a"));
  EXPECT_EQ(h.selectCount(ftn("c"), idOf(sub)), 1);
  EXPECT_EQ(h.selectCount(ftn("d"), idOf(sub)), 0);
  EXPECT_EQ(h.selectCount(ftn("b"), idOf(sub)), 0); // b was already selected
}

// ---------------------------------------------------------------------------
// sweepIdle + updateSortValue interaction tests
// ---------------------------------------------------------------------------

// When a value change triggers both recomputeTopNGroups() and sweepIdle() inside
// updateSortValue(), the track that enters top-N must be notified exactly once.
// recomputeTopNGroups runs first and selects b; sweepIdle runs after and must
// not re-notify b because it is already Selected.
TEST_F(PropertyRankingBaseTest, UpdateSortValue_SweepIdleAndRecomputeDoNotDoubleSelect) {
  // idleTimeout=100ms so sweepIdle is active. maxDeselected=0 (immediate evict).
  RankingHarness h(0, std::chrono::milliseconds(100));
  auto sub = makeParticipant();

  // a (100) is top-1; b (50) is outside. Neither has activity → both "never published".
  h.ranking().registerTrack(ftn("a"), 100, idOf(defaultPublisher_));
  h.ranking().registerTrack(ftn("b"), 50, idOf(defaultPublisher_));
  h.ranking().addSubscriberToTopNGroup(1, sub, true);
  h.clearEvents();

  // Stamp b's activity so only a is idle.
  h.setActivityTime(ftn("b"), std::chrono::steady_clock::now());

  // b's value crosses the N=1 threshold (rank 1→0).
  // recomputeTopNGroups selects b and demotes a; sweepIdle then runs but finds
  // b active and a already deselected — b must be selected exactly once.
  h.ranking().updateSortValue(ftn("b"), 200);

  EXPECT_EQ(h.selectCount(ftn("b")), 1);
}

TEST_F(PropertyRankingBaseTest, SweepThrottle_SkipsRunIfCalledTooSoon) {
  // sweepThrottle=500ms: a second sweepIdle() call within that window is a no-op.
  RankingHarness h(0, std::chrono::milliseconds(100), std::chrono::milliseconds(500));
  auto sub = makeParticipant();

  h.ranking().registerTrack(ftn("a"), 100, idOf(defaultPublisher_));
  h.ranking().registerTrack(ftn("b"), 90, idOf(defaultPublisher_));
  h.ranking().addSubscriberToTopNGroup(1, sub, true);
  h.clearEvents();

  // a is idle, b is active.
  h.setActivityTime(ftn("b"), std::chrono::steady_clock::now());

  // First sweep: a is idle → evicted, b promoted.
  h.ranking().sweepIdle();
  EXPECT_EQ(h.selectCount(ftn("b")), 1);

  // b goes idle, so only the throttle keeps the second sweep from evicting it.
  h.setActivityTime(ftn("b"), std::chrono::steady_clock::now() - std::chrono::seconds(1));
  h.clearEvents();

  h.ranking().sweepIdle();
  EXPECT_EQ(h.evictCount(ftn("b")), 0);
}

// sweepIdle tests
// ---------------------------------------------------------------------------

TEST_F(PropertyRankingBaseTest, SweepIdle_IdleTrackEvictedAndReplacementPromoted) {
  // Setup: 3 tracks, N=2 group, idleTimeout=100ms
  RankingHarness h(5, std::chrono::milliseconds(100));
  auto sub = makeParticipant();

  h.ranking().registerTrack(ftn("a"), 100, idOf(defaultPublisher_)); // rank 0
  h.ranking().registerTrack(ftn("b"), 90, idOf(defaultPublisher_));  // rank 1
  h.ranking().registerTrack(ftn("c"), 80, idOf(defaultPublisher_));  // rank 2

  h.ranking().addSubscriberToTopNGroup(2, sub, true);
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

TEST_F(PropertyRankingBaseTest, SweepIdle_TrackThatNeverPublishedIsTreatedAsIdle) {
  // A track with epoch timestamp (never published) should be evicted
  RankingHarness h(5, std::chrono::milliseconds(100));
  auto sub = makeParticipant();

  h.ranking().registerTrack(ftn("a"), 100, idOf(defaultPublisher_)); // rank 0
  h.ranking()
      .registerTrack(ftn("b"), 90, idOf(defaultPublisher_)); // rank 1 - never sets activity time
  h.ranking().registerTrack(ftn("c"), 80, idOf(defaultPublisher_)); // rank 2

  h.ranking().addSubscriberToTopNGroup(2, sub, true);
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

TEST_F(PropertyRankingBaseTest, SweepIdle_NoEvictionWhenIdleTimeoutIsZero) {
  // idleTimeout=0 disables idle eviction
  RankingHarness h(5, std::chrono::milliseconds(0));
  auto sub = makeParticipant();

  h.ranking().registerTrack(ftn("a"), 100, idOf(defaultPublisher_));
  h.ranking().registerTrack(ftn("b"), 90, idOf(defaultPublisher_));

  h.ranking().addSubscriberToTopNGroup(2, sub, true);
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

TEST_F(PropertyRankingBaseTest, SweepIdle_ActiveTracksNotEvicted) {
  RankingHarness h(5, std::chrono::milliseconds(100));
  auto sub = makeParticipant();

  h.ranking().registerTrack(ftn("a"), 100, idOf(defaultPublisher_));
  h.ranking().registerTrack(ftn("b"), 90, idOf(defaultPublisher_));

  h.ranking().addSubscriberToTopNGroup(2, sub, true);
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
