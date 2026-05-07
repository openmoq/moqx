/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/PropertyRanking.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <moxygen/test/MockMoQSession.h>

#include <memory>
#include <vector>

using namespace openmoq::moqx;
using namespace moxygen;
using ::testing::NiceMock;

namespace {

// Helpers
FullTrackName ftn(const std::string& ns, const std::string& name) {
  return FullTrackName{TrackNamespace{{ns}}, name};
}

class PropertyRankingSelfExclusionTest : public ::testing::Test {
protected:
  struct SelectRecord {
    FullTrackName ftn;
    MoQSession* session;
    bool forward;
  };

  struct EvictRecord {
    FullTrackName ftn;
    MoQSession* session;
  };

  std::vector<SelectRecord> selected_;
  std::vector<EvictRecord> evicted_;

  // PropertyRanking only stores shared_ptr<MoQSession> as map keys and passes
  // them back through callbacks — it never calls any MoQSession methods itself.
  // MockMoQSession has a default constructor for exactly this use case.
  std::shared_ptr<MoQSession> makeSession() {
    auto s = std::make_shared<NiceMock<moxygen::test::MockMoQSession>>();
    sessions_.push_back(s);
    return s;
  }

  std::unique_ptr<PropertyRanking> makeRanking(uint64_t maxDeselected = 100) {
    return std::make_unique<PropertyRanking>(
        /*propertyType=*/1,
        maxDeselected,
        /*idleTimeout=*/std::chrono::milliseconds{0},
        /*sweepThrottle=*/std::chrono::milliseconds{0},
        /*getLastActivity=*/
        [](const FullTrackName&) { return std::chrono::steady_clock::time_point{}; },
        /*onBatchSelected=*/
        [this](
            const FullTrackName& f,
            const std::vector<std::pair<std::shared_ptr<MoQSession>, bool>>& sessions
        ) {
          for (auto& [s, fwd] : sessions) {
            selected_.push_back({f, s.get(), fwd});
          }
        },
        /*onSelected=*/
        [this](const FullTrackName& f, std::shared_ptr<MoQSession> s, bool fwd) {
          selected_.push_back({f, s.get(), fwd});
        },
        /*onEvicted=*/
        [this](const FullTrackName& f, std::shared_ptr<MoQSession> s) {
          evicted_.push_back({f, s.get()});
        }
    );
  }

  bool wasSelected(const FullTrackName& f, MoQSession* s) {
    for (auto& r : selected_) {
      if (r.ftn == f && r.session == s) {
        return true;
      }
    }
    return false;
  }

  int countSelected(const FullTrackName& f, MoQSession* s) {
    int n = 0;
    for (auto& r : selected_) {
      if (r.ftn == f && r.session == s) {
        n++;
      }
    }
    return n;
  }

  bool wasEvicted(const FullTrackName& f, MoQSession* s) {
    for (auto& r : evicted_) {
      if (r.ftn == f && r.session == s) {
        return true;
      }
    }
    return false;
  }

  std::vector<std::shared_ptr<NiceMock<moxygen::test::MockMoQSession>>> sessions_;
  // Placeholder publisher for tracks whose publisher identity doesn't matter.
  std::shared_ptr<MoQSession> defaultPublisher_{makeSession()};
};

// Publisher subscribes with TRACK_FILTER: own track is never notified.
TEST_F(PropertyRankingSelfExclusionTest, PublisherDoesNotReceiveOwnTrack) {
  auto r = makeRanking();
  auto pub = makeSession();
  auto track = ftn("ns", "self");

  r->registerTrack(track, 100, pub);
  r->addSessionToTopNGroup(3, pub, true);

  EXPECT_FALSE(wasSelected(track, pub.get()));
}

// Viewer in the same group DOES receive the publisher's track.
TEST_F(PropertyRankingSelfExclusionTest, ViewerReceivesPublisherTrack) {
  auto r = makeRanking();
  auto pub = makeSession();
  auto viewer = makeSession();
  auto track = ftn("ns", "self");

  r->registerTrack(track, 100, pub);
  r->addSessionToTopNGroup(3, pub, true);
  r->addSessionToTopNGroup(3, viewer, true);

  EXPECT_FALSE(wasSelected(track, pub.get()));
  EXPECT_TRUE(wasSelected(track, viewer.get()));
}

// Mixed group: viewer sees all top-N; publisher-subscriber sees top-N
// excluding its own track.
TEST_F(PropertyRankingSelfExclusionTest, MixedGroup_ViewerAndPublisherSubscriber) {
  auto r = makeRanking();
  auto pub = makeSession();
  auto viewer = makeSession();

  auto self = ftn("ns", "pub-track");
  auto other1 = ftn("ns", "other1");
  auto other2 = ftn("ns", "other2");

  r->registerTrack(other1, 90, defaultPublisher_);
  r->registerTrack(other2, 80, defaultPublisher_);
  r->registerTrack(self, 70, pub);

  // N=3: viewer sees all 3; publisher sees only the 2 non-self tracks.
  r->addSessionToTopNGroup(3, pub, true);
  r->addSessionToTopNGroup(3, viewer, true);

  EXPECT_TRUE(wasSelected(other1, viewer.get()));
  EXPECT_TRUE(wasSelected(other2, viewer.get()));
  EXPECT_TRUE(wasSelected(self, viewer.get()));

  EXPECT_TRUE(wasSelected(other1, pub.get()));
  EXPECT_TRUE(wasSelected(other2, pub.get()));
  EXPECT_FALSE(wasSelected(self, pub.get()));
}

// computeWaterlineKey: publisher with N=2 and 1 self-track should be notified
// of exactly 2 non-self tracks, with the waterline at the 2nd non-self.
TEST_F(PropertyRankingSelfExclusionTest, WaterlineSelectsNonSelfTracks) {
  auto r = makeRanking();
  auto pub = makeSession();

  auto self = ftn("ns", "self");
  auto t1 = ftn("ns", "t1");
  auto t2 = ftn("ns", "t2");
  auto t3 = ftn("ns", "t3"); // Rank 3 (worst) — should NOT be selected (N=2)

  // Register with descending values so order is t1 > t2 > self > t3.
  r->registerTrack(t1, 100, defaultPublisher_);
  r->registerTrack(t2, 80, defaultPublisher_);
  r->registerTrack(self, 70, pub);
  r->registerTrack(t3, 60, defaultPublisher_);

  r->addSessionToTopNGroup(2, pub, true);

  // N=2 non-self: t1 (rank 0) and t2 (rank 1). t3 is the 3rd non-self → excluded.
  EXPECT_TRUE(wasSelected(t1, pub.get()));
  EXPECT_TRUE(wasSelected(t2, pub.get()));
  EXPECT_FALSE(wasSelected(self, pub.get()));
  EXPECT_FALSE(wasSelected(t3, pub.get()));
}

// Publisher with >1 self-track: waterline computed over non-self tracks only.
TEST_F(PropertyRankingSelfExclusionTest, MultipleSelfTracks_WaterlineCorrect) {
  auto r = makeRanking();
  auto pub = makeSession();

  auto s1 = ftn("ns", "s1");
  auto s2 = ftn("ns", "s2");
  auto t1 = ftn("ns", "t1");
  auto t2 = ftn("ns", "t2");
  auto t3 = ftn("ns", "t3");

  // Values: t1=100, s1=90, t2=80, s2=70, t3=60
  r->registerTrack(t1, 100, defaultPublisher_);
  r->registerTrack(s1, 90, pub);
  r->registerTrack(t2, 80, defaultPublisher_);
  r->registerTrack(s2, 70, pub);
  r->registerTrack(t3, 60, defaultPublisher_);

  // N=2 non-self: publisher should receive t1 and t2 only.
  r->addSessionToTopNGroup(2, pub, true);

  EXPECT_TRUE(wasSelected(t1, pub.get()));
  EXPECT_TRUE(wasSelected(t2, pub.get()));
  EXPECT_FALSE(wasSelected(s1, pub.get()));
  EXPECT_FALSE(wasSelected(s2, pub.get()));
  EXPECT_FALSE(wasSelected(t3, pub.get()));
}

// Self-track value change: waterline is recomputed, and the selection set
// for the publisher updates accordingly.
// Scenario: N=2, 2 self-tracks interspersed with non-self tracks.
// Initial order: t1(100) > s1(90) > t2(80) > s2(70) > t3(60).
// Non-self top-2: t1, t2. Publisher sees t1, t2.
// s1 drops to 50 → new order: t1(100) > t2(80) > s2(70) > t3(60) > s1(50).
// Non-self top-2 is still t1, t2. Publisher still sees t1, t2. No change.
TEST_F(PropertyRankingSelfExclusionTest, SelfTrackValueChange_NoEffectOnViewSet) {
  auto r = makeRanking();
  auto pub = makeSession();

  auto s1 = ftn("ns", "s1");
  auto s2 = ftn("ns", "s2");
  auto t1 = ftn("ns", "t1");
  auto t2 = ftn("ns", "t2");
  auto t3 = ftn("ns", "t3");

  r->registerTrack(t1, 100, defaultPublisher_);
  r->registerTrack(s1, 90, pub);
  r->registerTrack(t2, 80, defaultPublisher_);
  r->registerTrack(s2, 70, pub);
  r->registerTrack(t3, 60, defaultPublisher_);

  r->addSessionToTopNGroup(2, pub, true);

  selected_.clear();
  // s1 drops from 90 to 50. Waterline recomputed: non-self top-2 still t1, t2.
  r->updateSortValue(s1, 50);

  // Publisher should not be spuriously notified.
  EXPECT_FALSE(wasSelected(t1, pub.get()));
  EXPECT_FALSE(wasSelected(t2, pub.get()));
  EXPECT_FALSE(wasSelected(t3, pub.get()));
  EXPECT_FALSE(wasSelected(s1, pub.get()));
}

// Self-track value change causes outsider to enter publisher's view.
// N=2, initial: t1(100) > t2(80) > self(70) > outsider(10).
// Non-self top-2: t1, t2. Publisher sees t1, t2.
// self rises to 95 → order: t1(100) > self(95) > t2(80) > outsider(10).
// Non-self top-2: t1 (rank 0), t2 (rank 2). outsider still rank 3 → not selected.
// Now self rises to 110 → order: self(110) > t1(100) > t2(80) > outsider(10).
// Non-self top-2: t1 (rank 1), t2 (rank 2). outsider still not selected. No change.
TEST_F(PropertyRankingSelfExclusionTest, SelfTrackRises_OutsiderStaysOut) {
  auto r = makeRanking();
  auto pub = makeSession();

  auto self = ftn("ns", "self");
  auto t1 = ftn("ns", "t1");
  auto t2 = ftn("ns", "t2");
  auto outsider = ftn("ns", "outsider");

  r->registerTrack(t1, 100, defaultPublisher_);
  r->registerTrack(t2, 80, defaultPublisher_);
  r->registerTrack(self, 70, pub);
  r->registerTrack(outsider, 10, defaultPublisher_);

  r->addSessionToTopNGroup(2, pub, true);
  EXPECT_TRUE(wasSelected(t1, pub.get()));
  EXPECT_TRUE(wasSelected(t2, pub.get()));
  EXPECT_FALSE(wasSelected(outsider, pub.get()));

  // Self rises through ranks — outsider should not be selected.
  selected_.clear();
  r->updateSortValue(self, 110);
  EXPECT_FALSE(wasSelected(outsider, pub.get()));
  EXPECT_FALSE(wasSelected(self, pub.get()));
}

// Outsider enters publisher's top-N because a non-self track drops out.
// N=2, initial: t1(100) > t2(80) > self(70) > outsider(10).
// Non-self top-2: t1, t2.
// t2 drops to 5 → order: t1(100) > self(70) > outsider(10) > t2(5).
// Non-self top-2: t1 (rank 0), outsider (rank 2). Publisher sees outsider now.
TEST_F(PropertyRankingSelfExclusionTest, NonSelfTrackDrops_OutsiderEntersPublisherView) {
  auto r = makeRanking();
  auto pub = makeSession();

  auto self = ftn("ns", "self");
  auto t1 = ftn("ns", "t1");
  auto t2 = ftn("ns", "t2");
  auto outsider = ftn("ns", "outsider");

  r->registerTrack(t1, 100, defaultPublisher_);
  r->registerTrack(t2, 80, defaultPublisher_);
  r->registerTrack(self, 70, pub);
  r->registerTrack(outsider, 10, defaultPublisher_);

  r->addSessionToTopNGroup(2, pub, true);
  EXPECT_FALSE(wasSelected(outsider, pub.get()));

  // t2 drops below outsider.
  selected_.clear();
  evicted_.clear();
  r->updateSortValue(t2, 5);
  EXPECT_TRUE(wasSelected(outsider, pub.get()));
  EXPECT_TRUE(wasEvicted(t2, pub.get())); // t2 left publisher's view — must be evicted
  EXPECT_FALSE(wasSelected(self, pub.get()));
}

// Publisher with self-tracks both inside and outside the shared top-N.
// N=3: shared top-3 = t1, s1 (self), t2. Publisher sees t1, t2 (excludes s1).
// s2 (self, rank 4) is outside shared top-3, so waterline = t2's key.
// t3 (rank 5, non-self) is below waterline → not selected for publisher.
TEST_F(PropertyRankingSelfExclusionTest, SelfTracksInsideAndOutsideTopN) {
  auto r = makeRanking();
  auto pub = makeSession();

  auto s1 = ftn("ns", "s1");
  auto s2 = ftn("ns", "s2");
  auto t1 = ftn("ns", "t1");
  auto t2 = ftn("ns", "t2");
  auto t3 = ftn("ns", "t3");

  // Order: t1(100) > s1(90) > t2(80) > s2(70) > t3(60)
  r->registerTrack(t1, 100, defaultPublisher_);
  r->registerTrack(s1, 90, pub);
  r->registerTrack(t2, 80, defaultPublisher_);
  r->registerTrack(s2, 70, pub);
  r->registerTrack(t3, 60, defaultPublisher_);

  // N=3: shared top-3 = t1, s1, t2.
  // Non-self top-3: t1 (rank 0), t2 (rank 2), t3 (rank 4).
  // Waterline = key of 3rd non-self = t3's key (value=60).
  r->addSessionToTopNGroup(3, pub, true);

  EXPECT_TRUE(wasSelected(t1, pub.get()));  // rank 0, non-self → selected
  EXPECT_FALSE(wasSelected(s1, pub.get())); // self → excluded
  EXPECT_TRUE(wasSelected(t2, pub.get()));  // rank 2, non-self → selected
  EXPECT_FALSE(wasSelected(s2, pub.get())); // self → excluded
  EXPECT_TRUE(wasSelected(t3, pub.get()));  // rank 4, 3rd non-self → at waterline → selected
}

// A track registered after the publisher has already subscribed enters the
// publisher's personal top-N and displaces the previous waterline track.
//
// Setup: t1(100), t2(80), self(70) registered; publisher subscribes N=2.
// Publisher sees t1, t2.  newcomer(90) registered → enters rank 1.
// Non-self top-2: t1, newcomer.  t2 displaced and must be evicted.
TEST_F(PropertyRankingSelfExclusionTest, RegisterTrack_AfterPublisherSubscribed) {
  auto r = makeRanking();
  auto pub = makeSession();

  auto self = ftn("ns", "self");
  auto t1 = ftn("ns", "t1");
  auto t2 = ftn("ns", "t2");
  auto newcomer = ftn("ns", "newcomer");

  r->registerTrack(t1, 100, defaultPublisher_);
  r->registerTrack(t2, 80, defaultPublisher_);
  r->registerTrack(self, 70, pub);

  r->addSessionToTopNGroup(2, pub, true);
  EXPECT_TRUE(wasSelected(t1, pub.get()));
  EXPECT_TRUE(wasSelected(t2, pub.get()));

  selected_.clear();
  evicted_.clear();

  // newcomer enters at rank 1 (above t2, below t1).
  r->registerTrack(newcomer, 90, defaultPublisher_);

  EXPECT_TRUE(wasSelected(newcomer, pub.get())); // newcomer entered publisher's top-2
  EXPECT_TRUE(wasEvicted(t2, pub.get()));        // t2 displaced
  EXPECT_FALSE(wasEvicted(t1, pub.get()));       // t1 stays
  EXPECT_FALSE(wasSelected(self, pub.get()));    // self never selected
}

// When a track in the publisher's personal selection is removed, the publisher
// receives an eviction and the replacement track is selected.
//
// Setup: t1(100), t2(80), self(70), t3(60).  Publisher with {self}, N=2.
// Publisher sees t1, t2.  removeTrack(t2) → self promoted to shared top-2;
// reconcile produces: evict t2, select t3 (new 2nd non-self); self excluded.
TEST_F(PropertyRankingSelfExclusionTest, RemoveTrack_WithPublisherInGroup) {
  auto r = makeRanking();
  auto pub = makeSession();

  auto self = ftn("ns", "self");
  auto t1 = ftn("ns", "t1");
  auto t2 = ftn("ns", "t2");
  auto t3 = ftn("ns", "t3");

  r->registerTrack(t1, 100, defaultPublisher_);
  r->registerTrack(t2, 80, defaultPublisher_);
  r->registerTrack(self, 70, pub);
  r->registerTrack(t3, 60, defaultPublisher_);

  r->addSessionToTopNGroup(2, pub, true);
  EXPECT_TRUE(wasSelected(t1, pub.get()));
  EXPECT_TRUE(wasSelected(t2, pub.get()));

  selected_.clear();
  evicted_.clear();

  r->removeTrack(t2);

  EXPECT_TRUE(wasEvicted(t2, pub.get()));     // t2 removed — evicted from publisher
  EXPECT_TRUE(wasSelected(t3, pub.get()));    // t3 is the new 2nd non-self
  EXPECT_FALSE(wasSelected(self, pub.get())); // self excluded even when promoted in shared view
  EXPECT_FALSE(wasEvicted(t1, pub.get()));    // t1 stays
}

// Two publisher-subscribers in the same group each exclude only their own
// self-track; they can still receive each other's tracks.
//
// s1(100) > s2(90) > t1(80) > t2(70) > t3(60).  N=3.
// pub1 {s1}: non-self top-3 = s2, t1, t2.
// pub2 {s2}: non-self top-3 = s1, t1, t2.
//
// After s1 drops to 55: pub2's non-self top-3 shifts to t1, t2, t3 — s1 evicted,
// t3 selected.  pub1's view is unchanged (s1 is its own self-track, skipped).
TEST_F(PropertyRankingSelfExclusionTest, TwoPublisherSessions_IndependentSelfExclusion) {
  auto r = makeRanking();
  auto pub1 = makeSession();
  auto pub2 = makeSession();

  auto s1 = ftn("ns", "s1");
  auto s2 = ftn("ns", "s2");
  auto t1 = ftn("ns", "t1");
  auto t2 = ftn("ns", "t2");
  auto t3 = ftn("ns", "t3");

  r->registerTrack(s1, 100, pub1);
  r->registerTrack(s2, 90, pub2);
  r->registerTrack(t1, 80, defaultPublisher_);
  r->registerTrack(t2, 70, defaultPublisher_);
  r->registerTrack(t3, 60, defaultPublisher_);

  r->addSessionToTopNGroup(3, pub1, true);
  r->addSessionToTopNGroup(3, pub2, true);

  // pub1 sees s2, t1, t2 (not s1).
  EXPECT_FALSE(wasSelected(s1, pub1.get()));
  EXPECT_TRUE(wasSelected(s2, pub1.get()));
  EXPECT_TRUE(wasSelected(t1, pub1.get()));
  EXPECT_TRUE(wasSelected(t2, pub1.get()));
  EXPECT_FALSE(wasSelected(t3, pub1.get()));

  // pub2 sees s1, t1, t2 (not s2).
  EXPECT_TRUE(wasSelected(s1, pub2.get()));
  EXPECT_FALSE(wasSelected(s2, pub2.get()));
  EXPECT_TRUE(wasSelected(t1, pub2.get()));
  EXPECT_TRUE(wasSelected(t2, pub2.get()));
  EXPECT_FALSE(wasSelected(t3, pub2.get()));

  // s1 drops out of non-self top-3 for pub2; t3 enters.
  // pub1 is unaffected (s1 is its self-track — its waterline doesn't change).
  selected_.clear();
  evicted_.clear();
  r->updateSortValue(s1, 55); // s1: rank 0 → rank 4 (below t3)

  EXPECT_TRUE(wasEvicted(s1, pub2.get()));   // s1 left pub2's view
  EXPECT_TRUE(wasSelected(t3, pub2.get()));  // t3 entered pub2's view
  EXPECT_FALSE(wasSelected(t3, pub1.get())); // pub1 unaffected
  EXPECT_FALSE(wasEvicted(s1, pub1.get()));  // pub1 never received s1
}

// Bug: non-self track rises above the publisher's waterline, displacing the
// previous Nth non-self track.  The displaced track must be evicted from the
// publisher's delivery; without that eviction the publisher receives N+1 tracks.
//
// N=2, initial: t1(100) > t2(80) > self(70) > outsider(30).
// Non-self top-2: t1, t2.  outsider rises to 85 → non-self top-2: t1, outsider.
// t2 must be evicted and outsider must be selected.
TEST_F(PropertyRankingSelfExclusionTest, NonSelfTrackRises_DisplacesWaterlineTrack) {
  auto r = makeRanking();
  auto pub = makeSession();

  auto self = ftn("ns", "self");
  auto t1 = ftn("ns", "t1");
  auto t2 = ftn("ns", "t2");
  auto outsider = ftn("ns", "outsider");

  r->registerTrack(t1, 100, defaultPublisher_);
  r->registerTrack(t2, 80, defaultPublisher_);
  r->registerTrack(self, 70, pub);
  r->registerTrack(outsider, 30, defaultPublisher_);

  r->addSessionToTopNGroup(2, pub, true);
  EXPECT_TRUE(wasSelected(t1, pub.get()));
  EXPECT_TRUE(wasSelected(t2, pub.get()));
  EXPECT_FALSE(wasSelected(outsider, pub.get()));

  selected_.clear();
  evicted_.clear();
  r->updateSortValue(outsider, 85); // outsider rises above t2

  EXPECT_TRUE(wasSelected(outsider, pub.get())); // outsider entered publisher's top-2
  EXPECT_TRUE(wasEvicted(t2, pub.get()));        // t2 displaced — must be evicted
  EXPECT_FALSE(wasSelected(self, pub.get()));    // self never selected
  EXPECT_FALSE(wasEvicted(t1, pub.get()));       // t1 stayed in top-2
}

// Viewer subscribes first, then registers a track (becoming a publisher).
// The newly-registered track becomes a self-track and must be excluded.
//
// Setup: t1(100), t2(80), t3(60) registered.  Session subscribes N=2 as viewer.
// Viewer sees t1, t2.  Session then publishes new track "self" at rank 1 (value=90).
// Session now is a publisher-subscriber. Non-self top-2: t1, t2.
// The new track "self" must be excluded and t3 stays out.
TEST_F(PropertyRankingSelfExclusionTest, ViewerBecomesPublisher_ByRegisteringTrack) {
  auto r = makeRanking();
  auto pub = makeSession();

  auto t1 = ftn("ns", "t1");
  auto t2 = ftn("ns", "t2");
  auto t3 = ftn("ns", "t3");
  auto self = ftn("ns", "self");

  r->registerTrack(t1, 100, defaultPublisher_);
  r->registerTrack(t2, 80, defaultPublisher_);
  r->registerTrack(t3, 60, defaultPublisher_);

  // Subscribe as viewer first (no self-tracks yet)
  r->addSessionToTopNGroup(2, pub, true);
  EXPECT_TRUE(wasSelected(t1, pub.get()));
  EXPECT_TRUE(wasSelected(t2, pub.get()));
  EXPECT_FALSE(wasSelected(t3, pub.get()));

  selected_.clear();
  evicted_.clear();

  // Now the session registers its own track at rank 1
  r->registerTrack(self, 90, pub);

  // The session is now a publisher-subscriber. Its new track is self and excluded.
  // Non-self top-2: still t1, t2. The session should not see "self" selected.
  EXPECT_FALSE(wasSelected(self, pub.get())); // self is excluded
  EXPECT_FALSE(wasEvicted(t1, pub.get()));    // t1 stays
  EXPECT_FALSE(wasEvicted(t2, pub.get()));    // t2 stays
  EXPECT_FALSE(wasSelected(t3, pub.get()));   // t3 stays out
}

// Viewer subscribes, then registers a track that IS currently being delivered.
// That track becomes a self-track and must be evicted; a replacement enters.
//
// Setup: t1(100), t2(80), t3(60) registered.  Session subscribes N=2 as viewer.
// Viewer sees t1, t2.  Session then removes t2 and re-registers it as its own.
// Session now is a publisher-subscriber. Non-self top-2: t1, t3.
// t2 must be evicted and t3 must be selected.
TEST_F(PropertyRankingSelfExclusionTest, ViewerBecomesPublisher_ClaimsDeliveredTrack) {
  auto r = makeRanking();
  auto pub = makeSession();

  auto t1 = ftn("ns", "t1");
  auto t2 = ftn("ns", "t2");
  auto t3 = ftn("ns", "t3");

  // t2 is initially registered with no publisher
  r->registerTrack(t1, 100, defaultPublisher_);
  r->registerTrack(t2, 80, defaultPublisher_);
  r->registerTrack(t3, 60, defaultPublisher_);

  // Subscribe as viewer first
  r->addSessionToTopNGroup(2, pub, true);
  EXPECT_TRUE(wasSelected(t1, pub.get()));
  EXPECT_TRUE(wasSelected(t2, pub.get()));
  EXPECT_FALSE(wasSelected(t3, pub.get()));

  // Remove t2 so we can re-register it with the publisher
  r->removeTrack(t2);

  selected_.clear();
  evicted_.clear();

  // Now the session registers t2 as its own track
  r->registerTrack(t2, 80, pub);

  // t2 is now a self-track → evicted. t3 enters as replacement.
  EXPECT_TRUE(wasEvicted(t2, pub.get()));  // t2 now self-track → evicted
  EXPECT_TRUE(wasSelected(t3, pub.get())); // t3 enters publisher's top-2
  EXPECT_FALSE(wasEvicted(t1, pub.get())); // t1 stays
}

// Track registers outside the shared top-N but within the publisher's
// personal top-N.  This is the scenario the else-branch in
// PropertyRanking::registerTrack was added to handle.
//
// N=2. Shared top-2: {t1(100), self(80)}.  Publisher subscribes: sees t1 only
// (self excluded).  newcomer(60) registers at rank 2 (== N) — outside the
// shared top-N, so viewers are unaffected.  However, self occupies a shared
// slot, so the publisher's personal top-2 reaches to rank 2.  Without the
// else-branch reconciliation the publisher would never learn about newcomer.
TEST_F(
    PropertyRankingSelfExclusionTest,
    RegisterTrack_OutsideSharedTopN_InsidePublisherPersonalView
) {
  auto r = makeRanking();
  auto pub = makeSession();
  auto viewer = makeSession();

  auto self = ftn("ns", "self");
  auto t1 = ftn("ns", "t1");
  auto newcomer = ftn("ns", "newcomer");

  r->registerTrack(t1, 100, defaultPublisher_);
  r->registerTrack(self, 80, pub);

  // Shared top-2: {t1, self}.  Publisher sees t1; viewer sees t1 and self.
  r->addSessionToTopNGroup(2, pub, true);
  r->addSessionToTopNGroup(2, viewer, true);

  EXPECT_TRUE(wasSelected(t1, pub.get()));
  EXPECT_FALSE(wasSelected(self, pub.get()));
  EXPECT_TRUE(wasSelected(t1, viewer.get()));
  EXPECT_TRUE(wasSelected(self, viewer.get()));

  selected_.clear();
  evicted_.clear();

  // newcomer lands at rank 2 (== N) — outside the shared top-N.
  // Viewer is unaffected. Publisher's personal top-2 now covers {t1, newcomer}.
  r->registerTrack(newcomer, 60, defaultPublisher_);

  EXPECT_TRUE(wasSelected(newcomer, pub.get()));     // publisher picks up newcomer
  EXPECT_FALSE(wasSelected(newcomer, viewer.get())); // viewer unaffected (rank >= N)
  EXPECT_FALSE(wasSelected(self, pub.get()));        // self remains excluded
  EXPECT_FALSE(wasEvicted(t1, pub.get()));           // t1 stays
}

// Publisher with an existing self-track registers a second self-track.
// The second self-track must be excluded, and there must be no spurious
// re-notifications for tracks already being delivered.
//
// t1(100) > s1(80) > t2(60). Publisher subscribes N=2: sees t1, t2.
// Publisher then registers s2(40). Non-self top-2 is still t1, t2.
// Neither t1 nor t2 should be re-notified; s2 must be excluded.
TEST_F(PropertyRankingSelfExclusionTest, Publisher_RegistersAdditionalSelfTrack_NoRenotify) {
  auto r = makeRanking();
  auto pub = makeSession();

  auto s1 = ftn("ns", "s1");
  auto s2 = ftn("ns", "s2");
  auto t1 = ftn("ns", "t1");
  auto t2 = ftn("ns", "t2");

  r->registerTrack(t1, 100, defaultPublisher_);
  r->registerTrack(s1, 80, pub);
  r->registerTrack(t2, 60, defaultPublisher_);

  r->addSessionToTopNGroup(2, pub, true);
  EXPECT_TRUE(wasSelected(t1, pub.get()));
  EXPECT_TRUE(wasSelected(t2, pub.get()));
  EXPECT_FALSE(wasSelected(s1, pub.get()));

  selected_.clear();
  evicted_.clear();

  // Register a second self-track. Non-self top-2 is still t1, t2 — no change.
  r->registerTrack(s2, 40, pub);

  EXPECT_EQ(countSelected(t1, pub.get()), 0); // not re-notified
  EXPECT_EQ(countSelected(t2, pub.get()), 0); // not re-notified
  EXPECT_FALSE(wasSelected(s1, pub.get()));   // s1 excluded
  EXPECT_FALSE(wasSelected(s2, pub.get()));   // s2 excluded
  EXPECT_TRUE(evicted_.empty());
}

// Publisher's last self-track is removed. The session becomes a viewer.
// Subsequent entries into the shared top-N must be delivered via the viewer
// batch path and must not require any publisher waterline tracking.
//
// t1(100) > t2(80) > self(60) > t3(40). N=2.
// self is rank 2 — NOT in the shared top-2. Publisher subscribes: sees t1, t2.
// removeTrack(self): no promotion (self was deselected). Session is now a viewer.
// t3 rises to 85: new rank order t1(100) > t3(85) > t2(80).
// t3 enters the shared top-2 → viewer batch fires for pub.
TEST_F(PropertyRankingSelfExclusionTest, RemoveSelfTrack_BecomesViewer_ReceivedAsViewer) {
  auto r = makeRanking();
  auto pub = makeSession();

  auto self = ftn("ns", "self");
  auto t1 = ftn("ns", "t1");
  auto t2 = ftn("ns", "t2");
  auto t3 = ftn("ns", "t3");

  r->registerTrack(t1, 100, defaultPublisher_);
  r->registerTrack(t2, 80, defaultPublisher_);
  r->registerTrack(self, 60, pub); // rank 2, outside top-2
  r->registerTrack(t3, 40, defaultPublisher_);

  r->addSessionToTopNGroup(2, pub, true);
  EXPECT_TRUE(wasSelected(t1, pub.get()));
  EXPECT_TRUE(wasSelected(t2, pub.get()));
  EXPECT_FALSE(wasSelected(self, pub.get()));
  EXPECT_FALSE(wasSelected(t3, pub.get()));

  selected_.clear();
  evicted_.clear();

  // Remove self-track. self was deselected (rank 2 >= N=2) so no promotion fires.
  r->removeTrack(self);

  EXPECT_TRUE(selected_.empty());
  EXPECT_TRUE(evicted_.empty());

  // t3 rises into the shared top-2, displacing t2.
  // pub is now a viewer: it receives t3 via batch.
  r->updateSortValue(t3, 85);

  EXPECT_TRUE(wasSelected(t3, pub.get())); // viewer batch fires
  EXPECT_FALSE(wasEvicted(t2, pub.get())); // viewers don't receive eviction callbacks
}

// Regression test for GitHub issue #188: crossesThreshold() fast-path was using
// selectionThreshold_ (max N) which ignored publisher-subscribers' extended window.
//
// The bug: when both oldRank and newRank are >= selectionThreshold_, the fast-path
// returns false. But a publisher with self-tracks has an extended window of
// N + selfTrackCount, so moves within that extended zone must still trigger
// recomputation.
//
// Setup: t1(100), self(90), t2(80), t3(70), t4(60). N=3.
// Shared top-3: t1, self, t2. selectionThreshold_ = 3.
// Publisher's top-3 non-self: t1 (rank 0), t2 (rank 2), t3 (rank 3).
// t4 is at rank 4 — outside the shared threshold but t3 is AT the boundary.
//
// t4 rises from 60 to 75: moves from rank 4 to rank 3, pushing t3 to rank 4.
// crossesThreshold(4, 3) with selectionThreshold_=3: both >= 3 → false (BUG!).
// With publisherExtendedThreshold_=4: old=4 >= 4, new=3 < 4 → true (FIXED).
//
// The publisher should see t4 selected and t3 evicted.
TEST_F(PropertyRankingSelfExclusionTest, Issue188_CrossesThreshold_ExtendedWindow) {
  auto r = makeRanking();
  auto pub = makeSession();

  auto self = ftn("ns", "self");
  auto t1 = ftn("ns", "t1");
  auto t2 = ftn("ns", "t2");
  auto t3 = ftn("ns", "t3");
  auto t4 = ftn("ns", "t4");

  // Register tracks: t1(100) > self(90) > t2(80) > t3(70) > t4(60)
  r->registerTrack(t1, 100, defaultPublisher_);
  r->registerTrack(self, 90, pub);
  r->registerTrack(t2, 80, defaultPublisher_);
  r->registerTrack(t3, 70, defaultPublisher_);
  r->registerTrack(t4, 60, defaultPublisher_);

  // Publisher subscribes N=3. Shared top-3: t1, self, t2.
  // Publisher's non-self top-3: t1 (rank 0), t2 (rank 2), t3 (rank 3).
  r->addSessionToTopNGroup(3, pub, true);

  EXPECT_TRUE(wasSelected(t1, pub.get()));
  EXPECT_FALSE(wasSelected(self, pub.get())); // excluded
  EXPECT_TRUE(wasSelected(t2, pub.get()));
  EXPECT_TRUE(wasSelected(t3, pub.get())); // 3rd non-self
  EXPECT_FALSE(wasSelected(t4, pub.get()));

  selected_.clear();
  evicted_.clear();

  // t4 rises from 60 to 75. New order: t1(100) > self(90) > t2(80) > t4(75) > t3(70).
  // t4: rank 4 → rank 3. t3: rank 3 → rank 4.
  // Publisher's new non-self top-3: t1, t2, t4. t3 is now rank 4 (4th non-self).
  r->updateSortValue(t4, 75);

  EXPECT_TRUE(wasSelected(t4, pub.get()));    // t4 entered publisher's top-3
  EXPECT_TRUE(wasEvicted(t3, pub.get()));     // t3 pushed out of publisher's top-3
  EXPECT_FALSE(wasSelected(self, pub.get())); // self still excluded
}

} // namespace
