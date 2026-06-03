/*
 * Copyright (c) OpenMOQ contributors.
 *
 * Simple N+X Top-N Tracker Load Test
 *
 * Unit-level load test for SimpleTopNTracker that validates correctness and
 * performance of the lock-free N+X design under scale:
 * - 100 panelists (pub-subscribers with self-exclusion)
 * - 10,000 pure subscribers
 * - High-frequency property value updates
 *
 * Unlike TrackFilterLoadTest.cpp which tests the full relay stack over the
 * network, this test directly exercises SimpleTopNTracker to measure:
 * - Snapshot rebuild performance under high update rates
 * - Lock-free read scalability with concurrent readers
 * - Self-exclusion correctness for pub-subscribers
 * - Fast rejection optimization effectiveness
 *
 * This enables isolated performance analysis of the Simple N+X algorithm
 * without network/transport overhead.
 */

#include "relay/SimpleTopNTracker.h"

#include <folly/Synchronized.h>
#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <folly/portability/GFlags.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <moxygen/test/MockMoQSession.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

using namespace testing;
using namespace moxygen;
using namespace openmoq::moqx;
using namespace std::chrono;

DEFINE_int32(load_panelists, 100, "Number of panelists (pub-subscribers)");
DEFINE_int32(load_subscribers, 10000, "Number of pure subscribers");
DEFINE_int32(load_top_n, 3, "Top-N value for selection");
DEFINE_int32(load_duration_ms, 5000, "Test duration in milliseconds");
DEFINE_int32(load_update_hz, 1000, "Property value update frequency");
DEFINE_bool(load_verbose, false, "Enable verbose output");

namespace {

const TrackNamespace kNs{{"loadtest", "audio"}};

FullTrackName ftn(const std::string& name) {
  return FullTrackName{kNs, name};
}

std::string trackNameForPanelist(int id) {
  return "panelist-" + std::to_string(id);
}

// Metrics collection for the load test
struct LoadTestMetrics {
  // Update metrics
  std::atomic<uint64_t> updatesPerformed{0};
  std::atomic<uint64_t> snapshotRebuilds{0};

  // Read metrics
  std::atomic<uint64_t> snapshotReads{0};
  std::atomic<uint64_t> topNQueries{0};
  std::atomic<uint64_t> fastRejections{0};
  std::atomic<uint64_t> fullScans{0};

  // Correctness metrics
  std::atomic<uint64_t> selfExclusionViolations{0};
  std::atomic<uint64_t> topNMismatches{0};

  // Timing
  steady_clock::time_point testStart;
  steady_clock::time_point testEnd;

  // Latency tracking (in microseconds)
  std::atomic<uint64_t> totalUpdateLatencyUs{0};
  std::atomic<uint64_t> totalQueryLatencyUs{0};
  std::atomic<uint64_t> maxUpdateLatencyUs{0};
  std::atomic<uint64_t> maxQueryLatencyUs{0};

  void recordUpdateLatency(uint64_t us) {
    totalUpdateLatencyUs += us;
    uint64_t current = maxUpdateLatencyUs.load();
    while (us > current && !maxUpdateLatencyUs.compare_exchange_weak(current, us)) {
    }
  }

  void recordQueryLatency(uint64_t us) {
    totalQueryLatencyUs += us;
    uint64_t current = maxQueryLatencyUs.load();
    while (us > current && !maxQueryLatencyUs.compare_exchange_weak(current, us)) {
    }
  }
};

class SimpleTopNTrackerLoadTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Create mock sessions for panelists
    for (int i = 0; i < FLAGS_load_panelists; ++i) {
      auto session = std::make_shared<NiceMock<moxygen::test::MockMoQSession>>();
      panelistSessions_.push_back(session);
    }

    // Create mock sessions for pure subscribers
    for (int i = 0; i < FLAGS_load_subscribers; ++i) {
      auto session = std::make_shared<NiceMock<moxygen::test::MockMoQSession>>();
      subscriberSessions_.push_back(session);
    }
  }

  std::shared_ptr<MoQSession> getPanelistSession(int id) {
    return std::static_pointer_cast<MoQSession>(panelistSessions_[id]);
  }

  std::shared_ptr<MoQSession> getSubscriberSession(int id) {
    return std::static_pointer_cast<MoQSession>(subscriberSessions_[id]);
  }

  std::vector<std::shared_ptr<NiceMock<moxygen::test::MockMoQSession>>> panelistSessions_;
  std::vector<std::shared_ptr<NiceMock<moxygen::test::MockMoQSession>>> subscriberSessions_;
  LoadTestMetrics metrics_;
};

TEST_F(SimpleTopNTrackerLoadTest, ScaleTest) {
  std::cout << "\n";
  std::cout << "================================================================================\n";
  std::cout << "              SIMPLE N+X TOP-N TRACKER LOAD TEST\n";
  std::cout << "================================================================================\n\n";

  std::cout << "Configuration:\n";
  std::cout << "  Panelists (pub-sub):  " << FLAGS_load_panelists << "\n";
  std::cout << "  Pure Subscribers:     " << FLAGS_load_subscribers << "\n";
  std::cout << "  Top-N:                " << FLAGS_load_top_n << "\n";
  std::cout << "  Duration:             " << FLAGS_load_duration_ms << "ms\n";
  std::cout << "  Update Rate:          " << FLAGS_load_update_hz << " Hz\n\n";

  // Create tracker with snapshot change callback
  SimpleTopNTracker tracker(
      /*propertyType=*/1,
      /*idleTimeout=*/std::chrono::milliseconds{0},
      /*sweepThrottle=*/std::chrono::milliseconds{0},
      /*getLastActivity=*/[](const FullTrackName&) { return steady_clock::now(); },
      /*onSnapshotChanged=*/
      [this](auto, auto, auto) { metrics_.snapshotRebuilds++; }
  );

  // Phase 1: Register all sessions
  std::cout << "Phase 1: Registering sessions...\n";

  // Add panelist sessions (pub-subscribers)
  for (int i = 0; i < FLAGS_load_panelists; ++i) {
    tracker.addSession(FLAGS_load_top_n, getPanelistSession(i), true);
  }
  std::cout << "  Registered " << FLAGS_load_panelists << " panelist sessions\n";

  // Add pure subscriber sessions
  for (int i = 0; i < FLAGS_load_subscribers; ++i) {
    tracker.addSession(FLAGS_load_top_n, getSubscriberSession(i), true);
  }
  std::cout << "  Registered " << FLAGS_load_subscribers << " subscriber sessions\n";

  // Phase 2: Register all tracks (one per panelist)
  std::cout << "\nPhase 2: Registering tracks...\n";

  for (int i = 0; i < FLAGS_load_panelists; ++i) {
    // Initial value: higher ID = lower rank (for deterministic ranking)
    uint64_t initialValue = FLAGS_load_panelists - i;
    tracker.registerTrack(ftn(trackNameForPanelist(i)), initialValue, getPanelistSession(i));
  }
  std::cout << "  Registered " << FLAGS_load_panelists << " tracks\n";

  // Phase 3: Run concurrent update and read load
  std::cout << "\nPhase 3: Running load test for " << FLAGS_load_duration_ms << "ms...\n";

  metrics_.testStart = steady_clock::now();
  std::atomic<bool> running{true};

  // Update thread: simulates high-frequency property value changes
  std::thread updateThread([&]() {
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> trackDist(0, FLAGS_load_panelists - 1);
    std::uniform_int_distribution<uint64_t> valueDist(1, 1000);

    auto updateInterval = microseconds(1000000 / FLAGS_load_update_hz);
    auto nextUpdate = steady_clock::now();

    while (running.load()) {
      auto now = steady_clock::now();
      if (now < nextUpdate) {
        std::this_thread::sleep_for(nextUpdate - now);
      }
      nextUpdate += updateInterval;

      int trackId = trackDist(rng);
      uint64_t newValue = valueDist(rng);

      auto start = steady_clock::now();
      tracker.updateSortValue(ftn(trackNameForPanelist(trackId)), newValue);
      auto end = steady_clock::now();

      metrics_.updatesPerformed++;
      metrics_.recordUpdateLatency(duration_cast<microseconds>(end - start).count());
    }
  });

  // Reader threads: simulate concurrent snapshot reads and top-N queries
  std::vector<std::thread> readerThreads;
  int numReaders = std::min(4, static_cast<int>(std::thread::hardware_concurrency()));

  for (int r = 0; r < numReaders; ++r) {
    readerThreads.emplace_back([&, r]() {
      std::mt19937 rng(r * 1000);
      std::uniform_int_distribution<int> sessionDist(0, FLAGS_load_subscribers - 1);

      while (running.load()) {
        // Lock-free snapshot read
        auto start = steady_clock::now();
        auto snapshot = tracker.loadSnapshot();
        auto end = steady_clock::now();

        metrics_.snapshotReads++;
        metrics_.recordQueryLatency(duration_cast<microseconds>(end - start).count());

        // Query top-N for a random subscriber
        int subId = sessionDist(rng);
        auto session = getSubscriberSession(subId);

        start = steady_clock::now();
        auto topN = tracker.computeTopNForSession(session, FLAGS_load_top_n, *snapshot);
        end = steady_clock::now();

        metrics_.topNQueries++;
        metrics_.recordQueryLatency(duration_cast<microseconds>(end - start).count());

        // Verify no self-tracks for panelists
        if (subId < FLAGS_load_panelists) {
          std::string selfTrack = trackNameForPanelist(subId);
          for (const auto& track : topN) {
            if (track.trackName == selfTrack) {
              metrics_.selfExclusionViolations++;
            }
          }
        }

        std::this_thread::yield();
      }
    });
  }

  // Wait for test duration
  std::this_thread::sleep_for(milliseconds(FLAGS_load_duration_ms));
  running.store(false);

  // Join threads
  updateThread.join();
  for (auto& t : readerThreads) {
    t.join();
  }

  metrics_.testEnd = steady_clock::now();

  // Phase 4: Verify final state correctness
  std::cout << "\nPhase 4: Verifying correctness...\n";

  tracker.flush();
  auto finalSnapshot = tracker.loadSnapshot();

  // Verify self-exclusion for all panelists
  for (int i = 0; i < FLAGS_load_panelists; ++i) {
    auto session = getPanelistSession(i);
    auto topN = tracker.computeTopNForSession(session, FLAGS_load_top_n, *finalSnapshot);

    std::string selfTrack = trackNameForPanelist(i);
    for (const auto& track : topN) {
      if (track.trackName == selfTrack) {
        metrics_.selfExclusionViolations++;
        if (FLAGS_load_verbose) {
          std::cout << "  ERROR: Panelist " << i << " received self-track\n";
        }
      }
    }
  }

  // Verify top-N size for pure subscribers
  for (int i = 0; i < std::min(100, FLAGS_load_subscribers); ++i) {
    auto session = getSubscriberSession(i);
    auto topN = tracker.computeTopNForSession(session, FLAGS_load_top_n, *finalSnapshot);

    size_t expectedSize =
        std::min(static_cast<size_t>(FLAGS_load_top_n), static_cast<size_t>(FLAGS_load_panelists));
    if (topN.size() != expectedSize) {
      metrics_.topNMismatches++;
      if (FLAGS_load_verbose) {
        std::cout << "  ERROR: Subscriber " << i << " got " << topN.size() << " tracks, expected "
                  << expectedSize << "\n";
      }
    }
  }

  // Test fast rejection optimization
  std::cout << "\nPhase 5: Testing fast rejection optimization...\n";

  for (int i = 0; i < FLAGS_load_panelists; ++i) {
    auto session = getPanelistSession(i);
    uint8_t lastSelfPos = tracker.computeLastSelfPosition(session, *finalSnapshot);

    for (size_t pos = 0; pos < finalSnapshot->size(); ++pos) {
      if (SimpleTopNTracker::mightBeInTopN(pos, FLAGS_load_top_n, lastSelfPos)) {
        metrics_.fullScans++;
      } else {
        metrics_.fastRejections++;
      }
    }
  }

  // Generate report
  std::cout << "\n";
  std::cout << "================================================================================\n";
  std::cout << "                           LOAD TEST REPORT\n";
  std::cout << "================================================================================\n\n";

  auto duration = duration_cast<milliseconds>(metrics_.testEnd - metrics_.testStart);
  double durationSec = duration.count() / 1000.0;

  std::cout << "THROUGHPUT METRICS\n";
  std::cout << "------------------\n";
  std::cout << "  Updates Performed:     " << metrics_.updatesPerformed << "\n";
  std::cout << "  Snapshot Rebuilds:     " << metrics_.snapshotRebuilds << "\n";
  std::cout << "  Snapshot Reads:        " << metrics_.snapshotReads << "\n";
  std::cout << "  Top-N Queries:         " << metrics_.topNQueries << "\n";
  std::cout << "  Update Rate:           " << std::fixed << std::setprecision(1)
            << (metrics_.updatesPerformed / durationSec) << " updates/sec\n";
  std::cout << "  Read Rate:             " << std::fixed << std::setprecision(1)
            << (metrics_.snapshotReads / durationSec) << " reads/sec\n";
  std::cout << "  Query Rate:            " << std::fixed << std::setprecision(1)
            << (metrics_.topNQueries / durationSec) << " queries/sec\n\n";

  std::cout << "LATENCY METRICS\n";
  std::cout << "---------------\n";
  if (metrics_.updatesPerformed > 0) {
    std::cout << "  Avg Update Latency:    "
              << (metrics_.totalUpdateLatencyUs / metrics_.updatesPerformed) << " us\n";
    std::cout << "  Max Update Latency:    " << metrics_.maxUpdateLatencyUs << " us\n";
  }
  if (metrics_.topNQueries > 0) {
    std::cout << "  Avg Query Latency:     "
              << (metrics_.totalQueryLatencyUs / (metrics_.snapshotReads + metrics_.topNQueries))
              << " us\n";
    std::cout << "  Max Query Latency:     " << metrics_.maxQueryLatencyUs << " us\n";
  }
  std::cout << "\n";

  std::cout << "FAST REJECTION OPTIMIZATION\n";
  std::cout << "---------------------------\n";
  uint64_t totalChecks = metrics_.fastRejections + metrics_.fullScans;
  if (totalChecks > 0) {
    double rejectionRate = 100.0 * metrics_.fastRejections / totalChecks;
    std::cout << "  Fast Rejections:       " << metrics_.fastRejections << "\n";
    std::cout << "  Full Scans Required:   " << metrics_.fullScans << "\n";
    std::cout << "  Rejection Rate:        " << std::fixed << std::setprecision(1) << rejectionRate
              << "%\n\n";
  }

  std::cout << "CORRECTNESS VERIFICATION\n";
  std::cout << "------------------------\n";
  std::cout << "  Self-Exclusion Violations: " << metrics_.selfExclusionViolations << "\n";
  std::cout << "  Top-N Size Mismatches:     " << metrics_.topNMismatches << "\n";
  std::cout << "  Status:                    "
            << (metrics_.selfExclusionViolations == 0 && metrics_.topNMismatches == 0 ? "PASSED"
                                                                                      : "FAILED")
            << "\n\n";

  std::cout << "SNAPSHOT STATE\n";
  std::cout << "--------------\n";
  std::cout << "  Snapshot Size:         " << finalSnapshot->size() << "\n";
  std::cout << "  Max N:                 " << static_cast<int>(tracker.maxN()) << "\n";
  std::cout << "  Max Tracks/Publisher:  " << static_cast<int>(tracker.maxTracksPerPublisher())
            << "\n";
  std::cout << "  Snapshot Version:      " << tracker.snapshotVersion() << "\n\n";

  std::cout
      << "================================================================================\n\n";

  // Assertions for test pass/fail
  EXPECT_EQ(metrics_.selfExclusionViolations, 0) << "Self-exclusion violations detected";
  EXPECT_EQ(metrics_.topNMismatches, 0) << "Top-N size mismatches detected";
}

TEST_F(SimpleTopNTrackerLoadTest, ConcurrentReadWriteStress) {
  // Stress test for lock-free read correctness under heavy concurrent writes

  SimpleTopNTracker tracker(
      /*propertyType=*/1,
      /*idleTimeout=*/std::chrono::milliseconds{0},
      /*sweepThrottle=*/std::chrono::milliseconds{0},
      /*getLastActivity=*/[](const FullTrackName&) { return steady_clock::now(); },
      /*onSnapshotChanged=*/[](auto, auto, auto) {}
  );

  // Register a single session and track
  auto session = getPanelistSession(0);
  tracker.addSession(FLAGS_load_top_n, session, true);
  tracker.registerTrack(ftn("stress-track"), 100, session);

  std::atomic<bool> running{true};
  std::atomic<uint64_t> readCount{0};
  std::atomic<uint64_t> writeCount{0};
  std::atomic<uint64_t> readErrors{0};

  // Writer thread: rapidly updates the same track
  std::thread writer([&]() {
    uint64_t value = 0;
    while (running.load()) {
      tracker.updateSortValue(ftn("stress-track"), value++);
      writeCount++;
    }
  });

  // Multiple reader threads: continuously read snapshots
  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i) {
    readers.emplace_back([&]() {
      while (running.load()) {
        auto snapshot = tracker.loadSnapshot();
        if (!snapshot || snapshot->empty()) {
          readErrors++;
        }
        readCount++;
      }
    });
  }

  // Run for 1 second
  std::this_thread::sleep_for(seconds(1));
  running.store(false);

  writer.join();
  for (auto& r : readers) {
    r.join();
  }

  std::cout << "Concurrent Read/Write Stress Test:\n";
  std::cout << "  Writes: " << writeCount << "\n";
  std::cout << "  Reads:  " << readCount << "\n";
  std::cout << "  Errors: " << readErrors << "\n";

  EXPECT_EQ(readErrors, 0) << "Lock-free reads produced invalid snapshots";
  EXPECT_GT(readCount, 0) << "No reads completed";
  EXPECT_GT(writeCount, 0) << "No writes completed";
}

TEST_F(SimpleTopNTrackerLoadTest, SelfExclusionScaleVerification) {
  // Verify self-exclusion works correctly at scale

  SimpleTopNTracker tracker(
      /*propertyType=*/1,
      /*idleTimeout=*/std::chrono::milliseconds{0},
      /*sweepThrottle=*/std::chrono::milliseconds{0},
      /*getLastActivity=*/[](const FullTrackName&) { return steady_clock::now(); },
      /*onSnapshotChanged=*/[](auto, auto, auto) {}
  );

  // Register all panelists
  for (int i = 0; i < FLAGS_load_panelists; ++i) {
    auto session = getPanelistSession(i);
    tracker.addSession(FLAGS_load_top_n, session, true);
    tracker.registerTrack(ftn(trackNameForPanelist(i)), FLAGS_load_panelists - i, session);
  }

  auto snapshot = tracker.loadSnapshot();

  // Verify every panelist's top-N excludes their own track
  int violations = 0;
  for (int i = 0; i < FLAGS_load_panelists; ++i) {
    auto session = getPanelistSession(i);
    auto topN = tracker.computeTopNForSession(session, FLAGS_load_top_n, *snapshot);

    std::string selfTrack = trackNameForPanelist(i);
    for (const auto& track : topN) {
      if (track.trackName == selfTrack) {
        violations++;
        std::cout << "  Violation: panelist-" << i << " received own track\n";
      }
    }

    // Also verify using isInTopNWithSelfExclusion
    uint8_t lastSelfPos = tracker.computeLastSelfPosition(session, *snapshot);
    bool selfInTopN = tracker.isInTopNWithSelfExclusion(
        ftn(selfTrack), session, FLAGS_load_top_n, *snapshot, lastSelfPos
    );
    if (selfInTopN) {
      violations++;
      std::cout << "  Violation (isInTopN): panelist-" << i << " self-track returned true\n";
    }
  }

  std::cout << "Self-Exclusion Scale Verification:\n";
  std::cout << "  Panelists checked: " << FLAGS_load_panelists << "\n";
  std::cout << "  Violations: " << violations << "\n";

  EXPECT_EQ(violations, 0) << "Self-exclusion violations at scale";
}

TEST_F(SimpleTopNTrackerLoadTest, FastRejectionEffectiveness) {
  // Measure fast rejection effectiveness with various snapshot sizes

  SimpleTopNTracker tracker(
      /*propertyType=*/1,
      /*idleTimeout=*/std::chrono::milliseconds{0},
      /*sweepThrottle=*/std::chrono::milliseconds{0},
      /*getLastActivity=*/[](const FullTrackName&) { return steady_clock::now(); },
      /*onSnapshotChanged=*/[](auto, auto, auto) {}
  );

  // Use a larger N to get bigger snapshots
  const int testN = 50;

  // Register panelists with scattered self-tracks
  for (int i = 0; i < FLAGS_load_panelists; ++i) {
    auto session = getPanelistSession(i);
    tracker.addSession(testN, session, true);
    // Scatter values to create varied positions
    uint64_t value = (i % 2 == 0) ? (FLAGS_load_panelists - i) : i;
    tracker.registerTrack(ftn(trackNameForPanelist(i)), value, session);
  }

  auto snapshot = tracker.loadSnapshot();

  std::cout << "\nFast Rejection Effectiveness Test:\n";
  std::cout << "  Snapshot size: " << snapshot->size() << "\n";
  std::cout << "  N value: " << testN << "\n\n";

  // Test rejection rates for different panelists
  uint64_t totalFastRejected = 0;
  uint64_t totalFullScans = 0;

  for (int i = 0; i < std::min(10, FLAGS_load_panelists); ++i) {
    auto session = getPanelistSession(i);
    uint8_t lastSelfPos = tracker.computeLastSelfPosition(session, *snapshot);

    uint64_t fastRejected = 0;
    uint64_t fullScans = 0;

    for (size_t pos = 0; pos < snapshot->size(); ++pos) {
      if (SimpleTopNTracker::mightBeInTopN(pos, testN, lastSelfPos)) {
        fullScans++;
      } else {
        fastRejected++;
      }
    }

    totalFastRejected += fastRejected;
    totalFullScans += fullScans;

    if (FLAGS_load_verbose) {
      double rate = (fastRejected + fullScans > 0)
                        ? (100.0 * fastRejected / (fastRejected + fullScans))
                        : 0;
      std::cout << "  Panelist " << i << ": lastSelfPos=" << static_cast<int>(lastSelfPos)
                << ", rejected=" << fastRejected << "/" << (fastRejected + fullScans) << " ("
                << std::fixed << std::setprecision(1) << rate << "%)\n";
    }
  }

  double overallRate = (totalFastRejected + totalFullScans > 0)
                           ? (100.0 * totalFastRejected / (totalFastRejected + totalFullScans))
                           : 0;

  std::cout << "  Overall fast rejection rate: " << std::fixed << std::setprecision(1)
            << overallRate << "%\n";

  // With scattered self-positions, we should still get reasonable rejection
  EXPECT_GT(overallRate, 20.0) << "Fast rejection rate too low";
}

} // namespace

int main(int argc, char* argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  return RUN_ALL_TESTS();
}
