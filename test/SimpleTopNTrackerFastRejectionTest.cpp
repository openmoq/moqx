/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * Tests for the fast rejection optimization (O(1) rejection) in SimpleTopNTracker.
 *
 * Covers:
 * - mightBeInTopN() O(1) fast rejection logic
 * - isInTopNWithSelfExclusion() combined fast-path and full-scan
 * - Cached self-position computation
 * - Edge cases: pure subscribers, all self-tracks at top, etc.
 */

#include "relay/SimpleTopNTracker.h"

#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <moxygen/test/MockMoQSession.h>

using namespace testing;
using namespace moxygen;
using namespace openmoq::moqx;

namespace {

const TrackNamespace kNs{{"test"}};

FullTrackName ftn(const std::string& name) {
  return FullTrackName{kNs, name};
}

class SimpleTopNTrackerFastRejectionTest : public ::testing::Test {
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
// mightBeInTopN static function tests
// ---------------------------------------------------------------------------

TEST_F(SimpleTopNTrackerFastRejectionTest, MightBeInTopN_BasicRejection) {
  // N=3, lastSelfPos=2 -> threshold = 5
  // Tracks at positions 0-5 MIGHT be in top-N
  // Tracks at positions 6+ are DEFINITELY NOT

  EXPECT_TRUE(SimpleTopNTracker::mightBeInTopN(0, 3, 2));
  EXPECT_TRUE(SimpleTopNTracker::mightBeInTopN(3, 3, 2));
  EXPECT_TRUE(SimpleTopNTracker::mightBeInTopN(5, 3, 2));
  EXPECT_FALSE(SimpleTopNTracker::mightBeInTopN(6, 3, 2));
  EXPECT_FALSE(SimpleTopNTracker::mightBeInTopN(100, 3, 2));
}

TEST_F(SimpleTopNTrackerFastRejectionTest, MightBeInTopN_NoSelfTracks) {
  // N=5, lastSelfPos=0 (no self tracks) -> threshold = 5
  // Pure subscriber case: positions 0-4 might be in top-5

  EXPECT_TRUE(SimpleTopNTracker::mightBeInTopN(0, 5, 0));
  EXPECT_TRUE(SimpleTopNTracker::mightBeInTopN(4, 5, 0));
  EXPECT_TRUE(SimpleTopNTracker::mightBeInTopN(5, 5, 0));
  EXPECT_FALSE(SimpleTopNTracker::mightBeInTopN(6, 5, 0));
}

TEST_F(SimpleTopNTrackerFastRejectionTest, MightBeInTopN_AllSelfAtTop) {
  // N=3, lastSelfPos=5 (5 self tracks) -> threshold = 8
  // Worst case: all self tracks at top, need to scan far

  EXPECT_TRUE(SimpleTopNTracker::mightBeInTopN(0, 3, 5));
  EXPECT_TRUE(SimpleTopNTracker::mightBeInTopN(7, 3, 5));
  EXPECT_TRUE(SimpleTopNTracker::mightBeInTopN(8, 3, 5));
  EXPECT_FALSE(SimpleTopNTracker::mightBeInTopN(9, 3, 5));
}

TEST_F(SimpleTopNTrackerFastRejectionTest, MightBeInTopN_N1) {
  // N=1, lastSelfPos=0 -> threshold = 1

  EXPECT_TRUE(SimpleTopNTracker::mightBeInTopN(0, 1, 0));
  EXPECT_TRUE(SimpleTopNTracker::mightBeInTopN(1, 1, 0));
  EXPECT_FALSE(SimpleTopNTracker::mightBeInTopN(2, 1, 0));
}

// ---------------------------------------------------------------------------
// computeLastSelfPosition tests
// ---------------------------------------------------------------------------

TEST_F(SimpleTopNTrackerFastRejectionTest, ComputeLastSelfPosition_NoSelfTracks) {
  SimpleTopNTracker tracker(
      /*propertyType=*/1,
      /*idleTimeout=*/std::chrono::milliseconds{0},
      /*sweepThrottle=*/std::chrono::milliseconds{0},
      /*getLastActivity=*/[](const FullTrackName&) { return std::chrono::steady_clock::now(); },
      /*onSnapshotChanged=*/[](auto, auto, auto) {}
  );

  auto pubSub = makeSession();
  auto otherPub = makeSession();

  tracker.addSession(10, pubSub, true);

  // All tracks from otherPub
  tracker.registerTrack(ftn("a"), 100, otherPub);
  tracker.registerTrack(ftn("b"), 90, otherPub);
  tracker.registerTrack(ftn("c"), 80, otherPub);

  auto snapshot = tracker.loadSnapshot();

  // pubSub has no self-tracks
  EXPECT_EQ(tracker.computeLastSelfPosition(pubSub, *snapshot), 0);
}

TEST_F(SimpleTopNTrackerFastRejectionTest, ComputeLastSelfPosition_SelfTracksScattered) {
  SimpleTopNTracker tracker(
      /*propertyType=*/1,
      /*idleTimeout=*/std::chrono::milliseconds{0},
      /*sweepThrottle=*/std::chrono::milliseconds{0},
      /*getLastActivity=*/[](const FullTrackName&) { return std::chrono::steady_clock::now(); },
      /*onSnapshotChanged=*/[](auto, auto, auto) {}
  );

  auto pubSub = makeSession();
  auto otherPub = makeSession();

  tracker.addSession(10, pubSub, true);

  // Mixed publishers: pubSub's tracks at positions 1 and 4
  tracker.registerTrack(ftn("a"), 100, otherPub);   // pos 0
  tracker.registerTrack(ftn("self1"), 90, pubSub);  // pos 1
  tracker.registerTrack(ftn("b"), 80, otherPub);    // pos 2
  tracker.registerTrack(ftn("c"), 70, otherPub);    // pos 3
  tracker.registerTrack(ftn("self2"), 60, pubSub);  // pos 4
  tracker.registerTrack(ftn("d"), 50, otherPub);    // pos 5

  auto snapshot = tracker.loadSnapshot();

  // Last self-track is at position 4
  EXPECT_EQ(tracker.computeLastSelfPosition(pubSub, *snapshot), 4);
}

TEST_F(SimpleTopNTrackerFastRejectionTest, ComputeLastSelfPosition_SelfTrackAtEnd) {
  SimpleTopNTracker tracker(
      /*propertyType=*/1,
      /*idleTimeout=*/std::chrono::milliseconds{0},
      /*sweepThrottle=*/std::chrono::milliseconds{0},
      /*getLastActivity=*/[](const FullTrackName&) { return std::chrono::steady_clock::now(); },
      /*onSnapshotChanged=*/[](auto, auto, auto) {}
  );

  auto pubSub = makeSession();
  auto otherPub = makeSession();

  tracker.addSession(10, pubSub, true);

  // Self track at the end of snapshot
  tracker.registerTrack(ftn("a"), 100, otherPub);  // pos 0
  tracker.registerTrack(ftn("b"), 90, otherPub);   // pos 1
  tracker.registerTrack(ftn("self"), 80, pubSub);  // pos 2

  auto snapshot = tracker.loadSnapshot();

  EXPECT_EQ(tracker.computeLastSelfPosition(pubSub, *snapshot), 2);
}

// ---------------------------------------------------------------------------
// isInTopNWithSelfExclusion tests
// ---------------------------------------------------------------------------

TEST_F(SimpleTopNTrackerFastRejectionTest, IsInTopNWithSelfExclusion_FastRejection) {
  SimpleTopNTracker tracker(
      /*propertyType=*/1,
      /*idleTimeout=*/std::chrono::milliseconds{0},
      /*sweepThrottle=*/std::chrono::milliseconds{0},
      /*getLastActivity=*/[](const FullTrackName&) { return std::chrono::steady_clock::now(); },
      /*onSnapshotChanged=*/[](auto, auto, auto) {}
  );

  auto pubSub = makeSession();
  auto otherPub = makeSession();

  tracker.addSession(10, pubSub, true);

  // Create tracks: pubSub's track at position 1
  tracker.registerTrack(ftn("a"), 100, otherPub);   // pos 0
  tracker.registerTrack(ftn("self"), 90, pubSub);   // pos 1 (self)
  tracker.registerTrack(ftn("b"), 80, otherPub);    // pos 2
  tracker.registerTrack(ftn("c"), 70, otherPub);    // pos 3
  tracker.registerTrack(ftn("d"), 60, otherPub);    // pos 4
  tracker.registerTrack(ftn("e"), 50, otherPub);    // pos 5
  tracker.registerTrack(ftn("f"), 40, otherPub);    // pos 6

  auto snapshot = tracker.loadSnapshot();

  // lastSelfPosition = 1, N = 3 -> threshold = 4
  uint8_t lastSelfPos = tracker.computeLastSelfPosition(pubSub, *snapshot);
  EXPECT_EQ(lastSelfPos, 1);

  // Tracks at positions 0-4 might be in top-3
  EXPECT_TRUE(tracker.isInTopNWithSelfExclusion(ftn("a"), pubSub, 3, *snapshot, lastSelfPos));
  EXPECT_FALSE(tracker.isInTopNWithSelfExclusion(ftn("self"), pubSub, 3, *snapshot, lastSelfPos));
  EXPECT_TRUE(tracker.isInTopNWithSelfExclusion(ftn("b"), pubSub, 3, *snapshot, lastSelfPos));
  EXPECT_TRUE(tracker.isInTopNWithSelfExclusion(ftn("c"), pubSub, 3, *snapshot, lastSelfPos));

  // Track d is at position 4, but after self-exclusion it's the 4th non-self track
  // (a=0, self=skip, b=1, c=2, d=3) -> d is NOT in top-3
  EXPECT_FALSE(tracker.isInTopNWithSelfExclusion(ftn("d"), pubSub, 3, *snapshot, lastSelfPos));

  // Tracks at positions beyond threshold are O(1) rejected
  EXPECT_FALSE(tracker.isInTopNWithSelfExclusion(ftn("f"), pubSub, 3, *snapshot, lastSelfPos));
}

TEST_F(SimpleTopNTrackerFastRejectionTest, IsInTopNWithSelfExclusion_PureSubscriber) {
  SimpleTopNTracker tracker(
      /*propertyType=*/1,
      /*idleTimeout=*/std::chrono::milliseconds{0},
      /*sweepThrottle=*/std::chrono::milliseconds{0},
      /*getLastActivity=*/[](const FullTrackName&) { return std::chrono::steady_clock::now(); },
      /*onSnapshotChanged=*/[](auto, auto, auto) {}
  );

  auto viewer = makeSession();
  auto pub = makeSession();

  tracker.addSession(10, viewer, true);

  tracker.registerTrack(ftn("a"), 100, pub);
  tracker.registerTrack(ftn("b"), 90, pub);
  tracker.registerTrack(ftn("c"), 80, pub);
  tracker.registerTrack(ftn("d"), 70, pub);

  auto snapshot = tracker.loadSnapshot();

  // Viewer has no self-tracks, lastSelfPos = 0
  uint8_t lastSelfPos = tracker.computeLastSelfPosition(viewer, *snapshot);
  EXPECT_EQ(lastSelfPos, 0);

  // Pure subscriber: no self-exclusion needed
  EXPECT_TRUE(tracker.isInTopNWithSelfExclusion(ftn("a"), viewer, 2, *snapshot, lastSelfPos));
  EXPECT_TRUE(tracker.isInTopNWithSelfExclusion(ftn("b"), viewer, 2, *snapshot, lastSelfPos));
  EXPECT_FALSE(tracker.isInTopNWithSelfExclusion(ftn("c"), viewer, 2, *snapshot, lastSelfPos));
  EXPECT_FALSE(tracker.isInTopNWithSelfExclusion(ftn("d"), viewer, 2, *snapshot, lastSelfPos));
}

TEST_F(SimpleTopNTrackerFastRejectionTest, IsInTopNWithSelfExclusion_NullSession) {
  SimpleTopNTracker tracker(
      /*propertyType=*/1,
      /*idleTimeout=*/std::chrono::milliseconds{0},
      /*sweepThrottle=*/std::chrono::milliseconds{0},
      /*getLastActivity=*/[](const FullTrackName&) { return std::chrono::steady_clock::now(); },
      /*onSnapshotChanged=*/[](auto, auto, auto) {}
  );

  auto pub = makeSession();
  tracker.addSession(10, pub, true);

  tracker.registerTrack(ftn("a"), 100, pub);
  tracker.registerTrack(ftn("b"), 90, pub);

  auto snapshot = tracker.loadSnapshot();

  // Null session: uses fast path without self-exclusion
  EXPECT_TRUE(tracker.isInTopNWithSelfExclusion(ftn("a"), nullptr, 1, *snapshot, 0));
  EXPECT_FALSE(tracker.isInTopNWithSelfExclusion(ftn("b"), nullptr, 1, *snapshot, 0));
}

TEST_F(SimpleTopNTrackerFastRejectionTest, IsInTopNWithSelfExclusion_TrackNotInSnapshot) {
  SimpleTopNTracker tracker(
      /*propertyType=*/1,
      /*idleTimeout=*/std::chrono::milliseconds{0},
      /*sweepThrottle=*/std::chrono::milliseconds{0},
      /*getLastActivity=*/[](const FullTrackName&) { return std::chrono::steady_clock::now(); },
      /*onSnapshotChanged=*/[](auto, auto, auto) {}
  );

  auto session = makeSession();
  auto pub = makeSession();

  tracker.addSession(10, session, true);

  tracker.registerTrack(ftn("a"), 100, pub);

  auto snapshot = tracker.loadSnapshot();

  // Track not in snapshot
  EXPECT_FALSE(tracker.isInTopNWithSelfExclusion(ftn("unknown"), session, 5, *snapshot, 0));
}

TEST_F(SimpleTopNTrackerFastRejectionTest, IsInTopNWithSelfExclusion_AllSelfAtTop) {
  SimpleTopNTracker tracker(
      /*propertyType=*/1,
      /*idleTimeout=*/std::chrono::milliseconds{0},
      /*sweepThrottle=*/std::chrono::milliseconds{0},
      /*getLastActivity=*/[](const FullTrackName&) { return std::chrono::steady_clock::now(); },
      /*onSnapshotChanged=*/[](auto, auto, auto) {}
  );

  auto pubSub = makeSession();
  auto otherPub = makeSession();

  tracker.addSession(10, pubSub, true);

  // PubSub's tracks dominate top positions
  tracker.registerTrack(ftn("self1"), 100, pubSub);  // pos 0 (self)
  tracker.registerTrack(ftn("self2"), 95, pubSub);   // pos 1 (self)
  tracker.registerTrack(ftn("self3"), 90, pubSub);   // pos 2 (self)
  tracker.registerTrack(ftn("a"), 85, otherPub);     // pos 3
  tracker.registerTrack(ftn("b"), 80, otherPub);     // pos 4
  tracker.registerTrack(ftn("c"), 75, otherPub);     // pos 5

  auto snapshot = tracker.loadSnapshot();

  uint8_t lastSelfPos = tracker.computeLastSelfPosition(pubSub, *snapshot);
  EXPECT_EQ(lastSelfPos, 2);

  // pubSub wants top-2: should get tracks a and b (first 2 non-self)
  EXPECT_FALSE(tracker.isInTopNWithSelfExclusion(ftn("self1"), pubSub, 2, *snapshot, lastSelfPos));
  EXPECT_FALSE(tracker.isInTopNWithSelfExclusion(ftn("self2"), pubSub, 2, *snapshot, lastSelfPos));
  EXPECT_FALSE(tracker.isInTopNWithSelfExclusion(ftn("self3"), pubSub, 2, *snapshot, lastSelfPos));
  EXPECT_TRUE(tracker.isInTopNWithSelfExclusion(ftn("a"), pubSub, 2, *snapshot, lastSelfPos));
  EXPECT_TRUE(tracker.isInTopNWithSelfExclusion(ftn("b"), pubSub, 2, *snapshot, lastSelfPos));
  EXPECT_FALSE(tracker.isInTopNWithSelfExclusion(ftn("c"), pubSub, 2, *snapshot, lastSelfPos));
}

// ---------------------------------------------------------------------------
// Performance characteristics (documentation)
// ---------------------------------------------------------------------------

TEST_F(SimpleTopNTrackerFastRejectionTest, FastRejectionStats) {
  // This test documents the expected fast rejection rate.
  // With N=10 and lastSelfPos=4 (5 self-tracks at top), threshold = 14.
  // If snapshot has 100 tracks, ~86% can be O(1) rejected.

  SimpleTopNTracker tracker(
      /*propertyType=*/1,
      /*idleTimeout=*/std::chrono::milliseconds{0},
      /*sweepThrottle=*/std::chrono::milliseconds{0},
      /*getLastActivity=*/[](const FullTrackName&) { return std::chrono::steady_clock::now(); },
      /*onSnapshotChanged=*/[](auto, auto, auto) {}
  );

  auto pubSub = makeSession();
  auto otherPub = makeSession();

  tracker.addSession(100, pubSub, true); // Large N to get big snapshot

  // Register 100 tracks: first 5 from pubSub (highest values), rest from otherPub
  for (int i = 0; i < 100; i++) {
    auto pub = (i < 5) ? pubSub : otherPub;
    tracker.registerTrack(ftn("track" + std::to_string(i)), 1000 - i, pub);
  }

  auto snapshot = tracker.loadSnapshot();
  uint8_t lastSelfPos = tracker.computeLastSelfPosition(pubSub, *snapshot);

  // Count how many tracks can be fast-rejected for N=10
  int fastRejected = 0;
  int needsScan = 0;

  for (size_t i = 0; i < snapshot->size(); i++) {
    if (!SimpleTopNTracker::mightBeInTopN(i, 10, lastSelfPos)) {
      fastRejected++;
    } else {
      needsScan++;
    }
  }

  // Should reject the majority with O(1)
  EXPECT_GT(fastRejected, needsScan)
      << "Fast rejection should eliminate majority of checks for large snapshot";

  // Document the ratio
  double rejectionRate = static_cast<double>(fastRejected) / snapshot->size() * 100;
  SUCCEED() << "Fast rejection rate: " << rejectionRate << "% ("
            << fastRejected << "/" << snapshot->size() << " tracks)";
}

} // namespace
