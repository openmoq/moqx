/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * Tests specific to the SimpleTopNTracker's lock-free design.
 *
 * Covers:
 * - Lock-free snapshot reads during concurrent writes
 * - Atomic snapshot swapping
 * - Single global max_n tracking
 * - computeTopNForSession with self-exclusion
 */

#include "relay/SimpleTopNTracker.h"

#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <moxygen/test/MockMoQSession.h>

#include <atomic>
#include <thread>

using namespace testing;
using namespace moxygen;
using namespace openmoq::moqx;

namespace {

const TrackNamespace kNs{{"test"}};

FullTrackName ftn(const std::string& name) {
  return FullTrackName{kNs, name};
}

class SimpleTopNTrackerLockFreeTest : public ::testing::Test {
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
// Lock-free snapshot reads
// ---------------------------------------------------------------------------

TEST_F(SimpleTopNTrackerLockFreeTest, SnapshotCanBeReadWithoutLocking) {
  std::atomic<int> changeCount{0};

  SimpleTopNTracker tracker(
      /*propertyType=*/1,
      /*idleTimeout=*/std::chrono::milliseconds{0},
      /*sweepThrottle=*/std::chrono::milliseconds{0},
      /*getLastActivity=*/[](const FullTrackName&) { return std::chrono::steady_clock::now(); },
      /*onSnapshotChanged=*/[&](auto, auto, auto) { changeCount.fetch_add(1); }
  );

  auto session = makeSession();
  tracker.addSession(5, session, true);

  // Register tracks
  for (int i = 0; i < 10; i++) {
    tracker.registerTrack(ftn("track" + std::to_string(i)), i * 10, defaultPublisher_);
  }

  // Read snapshot (lock-free)
  auto snapshot = tracker.loadSnapshot();
  ASSERT_NE(snapshot, nullptr);
  // Snapshot size is max_n * 2 to allow for self-exclusion
  EXPECT_GE(snapshot->size(), 5u);

  // Verify sorting
  for (size_t i = 1; i < snapshot->size(); i++) {
    EXPECT_GE((*snapshot)[i - 1].propertyValue, (*snapshot)[i].propertyValue);
  }
}

TEST_F(SimpleTopNTrackerLockFreeTest, SnapshotRemainsValidDuringUpdates) {
  SimpleTopNTracker tracker(
      /*propertyType=*/1,
      /*idleTimeout=*/std::chrono::milliseconds{0},
      /*sweepThrottle=*/std::chrono::milliseconds{0},
      /*getLastActivity=*/[](const FullTrackName&) { return std::chrono::steady_clock::now(); },
      /*onSnapshotChanged=*/[](auto, auto, auto) {}
  );

  auto session = makeSession();
  tracker.addSession(3, session, true);

  tracker.registerTrack(ftn("a"), 100, defaultPublisher_);
  tracker.registerTrack(ftn("b"), 50, defaultPublisher_);

  // Get snapshot before update
  auto snapshotBefore = tracker.loadSnapshot();
  ASSERT_GE(snapshotBefore->size(), 2u);
  EXPECT_EQ((*snapshotBefore)[0].propertyValue, 100u);

  // Update track value (deferred — marks dirty)
  tracker.updateSortValue(ftn("b"), 200);
  tracker.flush();

  // Old snapshot is still valid and unchanged (copy-on-write)
  EXPECT_EQ((*snapshotBefore)[0].propertyValue, 100u);
  EXPECT_EQ((*snapshotBefore)[1].propertyValue, 50u);

  // New snapshot reflects update
  auto snapshotAfter = tracker.loadSnapshot();
  EXPECT_EQ((*snapshotAfter)[0].propertyValue, 200u);
  EXPECT_EQ((*snapshotAfter)[1].propertyValue, 100u);
}

// ---------------------------------------------------------------------------
// Global max_n tracking
// ---------------------------------------------------------------------------

TEST_F(SimpleTopNTrackerLockFreeTest, MaxNTracksLargestSubscriberN) {
  SimpleTopNTracker tracker(
      /*propertyType=*/1,
      /*idleTimeout=*/std::chrono::milliseconds{0},
      /*sweepThrottle=*/std::chrono::milliseconds{0},
      /*getLastActivity=*/[](const FullTrackName&) { return std::chrono::steady_clock::now(); },
      /*onSnapshotChanged=*/[](auto, auto, auto) {}
  );

  auto session1 = makeSession();
  auto session2 = makeSession();
  auto session3 = makeSession();

  tracker.addSession(3, session1, true);
  EXPECT_EQ(tracker.maxN(), 3);

  tracker.addSession(7, session2, true);
  EXPECT_EQ(tracker.maxN(), 7);

  tracker.addSession(5, session3, true);
  EXPECT_EQ(tracker.maxN(), 7); // Still 7

  tracker.removeSession(7, session2);
  EXPECT_EQ(tracker.maxN(), 5); // Now 5

  tracker.removeSession(5, session3);
  EXPECT_EQ(tracker.maxN(), 3); // Back to 3

  tracker.removeSession(3, session1);
  EXPECT_EQ(tracker.maxN(), 0); // No subscribers
}

// ---------------------------------------------------------------------------
// computeTopNForSession with self-exclusion
// ---------------------------------------------------------------------------

TEST_F(SimpleTopNTrackerLockFreeTest, ComputeTopNForSession_ExcludesSelf) {
  SimpleTopNTracker tracker(
      /*propertyType=*/1,
      /*idleTimeout=*/std::chrono::milliseconds{0},
      /*sweepThrottle=*/std::chrono::milliseconds{0},
      /*getLastActivity=*/[](const FullTrackName&) { return std::chrono::steady_clock::now(); },
      /*onSnapshotChanged=*/[](auto, auto, auto) {}
  );

  auto pubSub = makeSession();
  auto otherPub = makeSession();

  tracker.addSession(10, pubSub, true); // max_n = 10

  // Register tracks: pubSub's track at rank 2
  tracker.registerTrack(ftn("t0"), 100, otherPub);
  tracker.registerTrack(ftn("t1"), 90, otherPub);
  tracker.registerTrack(ftn("self"), 85, pubSub); // pubSub's track
  tracker.registerTrack(ftn("t2"), 80, otherPub);
  tracker.registerTrack(ftn("t3"), 70, otherPub);
  tracker.registerTrack(ftn("t4"), 60, otherPub);

  auto snapshot = tracker.loadSnapshot();

  // Compute top-3 for pubSub - should skip "self" and return t0, t1, t2
  auto result = tracker.computeTopNForSession(pubSub, 3, *snapshot);

  EXPECT_EQ(result.size(), 3u);
  EXPECT_EQ(result[0], ftn("t0"));
  EXPECT_EQ(result[1], ftn("t1"));
  EXPECT_EQ(result[2], ftn("t2")); // t2 promoted because "self" excluded

  // Verify "self" is not in the result
  for (const auto& f : result) {
    EXPECT_NE(f, ftn("self"));
  }
}

TEST_F(SimpleTopNTrackerLockFreeTest, ComputeTopNForSession_NoSelfTracks) {
  SimpleTopNTracker tracker(
      /*propertyType=*/1,
      /*idleTimeout=*/std::chrono::milliseconds{0},
      /*sweepThrottle=*/std::chrono::milliseconds{0},
      /*getLastActivity=*/[](const FullTrackName&) { return std::chrono::steady_clock::now(); },
      /*onSnapshotChanged=*/[](auto, auto, auto) {}
  );

  auto viewer = makeSession();
  auto pub = makeSession();

  tracker.addSession(3, viewer, true);

  tracker.registerTrack(ftn("a"), 100, pub);
  tracker.registerTrack(ftn("b"), 90, pub);
  tracker.registerTrack(ftn("c"), 80, pub);

  auto snapshot = tracker.loadSnapshot();

  // Viewer has no self-tracks, should get exactly top-3
  auto result = tracker.computeTopNForSession(viewer, 3, *snapshot);

  EXPECT_EQ(result.size(), 3u);
  EXPECT_EQ(result[0], ftn("a"));
  EXPECT_EQ(result[1], ftn("b"));
  EXPECT_EQ(result[2], ftn("c"));
}

TEST_F(SimpleTopNTrackerLockFreeTest, ShouldForward_ChecksAgainstSubscriberN) {
  SimpleTopNTracker tracker(
      /*propertyType=*/1,
      /*idleTimeout=*/std::chrono::milliseconds{0},
      /*sweepThrottle=*/std::chrono::milliseconds{0},
      /*getLastActivity=*/[](const FullTrackName&) { return std::chrono::steady_clock::now(); },
      /*onSnapshotChanged=*/[](auto, auto, auto) {}
  );

  auto session = makeSession();
  tracker.addSession(10, session, true); // max_n = 10

  // Register tracks with values 100, 90, 80, 70, 60
  tracker.registerTrack(ftn("t0"), 100, defaultPublisher_);
  tracker.registerTrack(ftn("t1"), 90, defaultPublisher_);
  tracker.registerTrack(ftn("t2"), 80, defaultPublisher_);
  tracker.registerTrack(ftn("t3"), 70, defaultPublisher_);
  tracker.registerTrack(ftn("t4"), 60, defaultPublisher_);

  auto snapshot = tracker.loadSnapshot();

  // Subscriber with N=3 should forward t0, t1, t2 (ranks 0, 1, 2)
  EXPECT_TRUE(tracker.shouldForward(ftn("t0"), 3, *snapshot));
  EXPECT_TRUE(tracker.shouldForward(ftn("t1"), 3, *snapshot));
  EXPECT_TRUE(tracker.shouldForward(ftn("t2"), 3, *snapshot));
  EXPECT_FALSE(tracker.shouldForward(ftn("t3"), 3, *snapshot));
  EXPECT_FALSE(tracker.shouldForward(ftn("t4"), 3, *snapshot));

  // Subscriber with N=1 should only forward t0
  EXPECT_TRUE(tracker.shouldForward(ftn("t0"), 1, *snapshot));
  EXPECT_FALSE(tracker.shouldForward(ftn("t1"), 1, *snapshot));

  // Subscriber with N=5 should forward all
  EXPECT_TRUE(tracker.shouldForward(ftn("t0"), 5, *snapshot));
  EXPECT_TRUE(tracker.shouldForward(ftn("t4"), 5, *snapshot));
}

// ---------------------------------------------------------------------------
// Concurrent read/write stress test
// ---------------------------------------------------------------------------

TEST_F(SimpleTopNTrackerLockFreeTest, ConcurrentReadsAndWrites) {
  std::atomic<int> changeCount{0};

  SimpleTopNTracker tracker(
      /*propertyType=*/1,
      /*idleTimeout=*/std::chrono::milliseconds{0},
      /*sweepThrottle=*/std::chrono::milliseconds{0},
      /*getLastActivity=*/[](const FullTrackName&) { return std::chrono::steady_clock::now(); },
      /*onSnapshotChanged=*/[&](auto, auto, auto) { changeCount.fetch_add(1); }
  );

  auto session = makeSession();
  tracker.addSession(10, session, true);

  // Register initial tracks
  for (int i = 0; i < 20; i++) {
    tracker.registerTrack(ftn("track" + std::to_string(i)), i * 5, defaultPublisher_);
  }

  std::atomic<bool> running{true};
  std::atomic<int> readCount{0};

  // Reader thread - continuously reads snapshots
  std::thread reader([&]() {
    while (running.load()) {
      auto snapshot = tracker.loadSnapshot();
      // Verify snapshot is valid and sorted
      if (snapshot && !snapshot->empty()) {
        for (size_t i = 1; i < snapshot->size(); i++) {
          EXPECT_GE((*snapshot)[i - 1].propertyValue, (*snapshot)[i].propertyValue);
        }
        readCount.fetch_add(1);
      }
    }
  });

  // Writer thread - updates track values
  for (int iter = 0; iter < 100; iter++) {
    int trackIdx = iter % 20;
    tracker.updateSortValue(ftn("track" + std::to_string(trackIdx)), (iter * 7) % 200);
  }

  running.store(false);
  reader.join();

  // Should have completed many reads while writes were happening
  EXPECT_GT(readCount.load(), 0);
}

// ---------------------------------------------------------------------------
// Memory efficiency
// ---------------------------------------------------------------------------

TEST_F(SimpleTopNTrackerLockFreeTest, MinimalPerSubscriberState) {
  SimpleTopNTracker tracker(
      /*propertyType=*/1,
      /*idleTimeout=*/std::chrono::milliseconds{0},
      /*sweepThrottle=*/std::chrono::milliseconds{0},
      /*getLastActivity=*/[](const FullTrackName&) { return std::chrono::steady_clock::now(); },
      /*onSnapshotChanged=*/[](auto, auto, auto) {}
  );

  // Add 100 subscribers with various N values
  std::vector<std::shared_ptr<MoQSession>> sessions;
  for (int i = 0; i < 100; i++) {
    auto s = makeSession();
    tracker.addSession((i % 10) + 1, s, true);
    sessions.push_back(s);
  }

  // Register 50 tracks
  for (int i = 0; i < 50; i++) {
    tracker.registerTrack(ftn("track" + std::to_string(i)), i * 10, defaultPublisher_);
  }

  // All 100 subscribers should be tracked
  EXPECT_EQ(tracker.numSessions(), 100u);

  // Snapshot should be at least max_n = 10 (may be larger for self-exclusion buffer)
  EXPECT_GE(tracker.loadSnapshot()->size(), 10u);

  // Track index has 50 entries
  EXPECT_EQ(tracker.numTracks(), 50u);
}

} // namespace
