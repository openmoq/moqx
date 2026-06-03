/*
 * Copyright (c) OpenMOQ contributors.
 *
 * Simple N+X Top-N Tracker Performance Test
 *
 * Multi-round performance test measuring CPU and memory overhead of the
 * Simple N+X top-N algorithm under realistic webinar-style speech patterns.
 *
 * Scenario: 50 pub-subscribers (panelists) with speech state machine,
 * 500 pure subscribers, configurable rounds.
 *
 * Outputs:
 * - Per-round JSON with CPU breakdown (rebuild, query, callback), memory,
 *   latency percentiles, fast-rejection stats
 * - TOPN_EVENT structured logs for visualization (topn_viz.py)
 *
 * Usage:
 *   ./simple_topn_tracker_perf_test \
 *     --perf_panelists=50 --perf_subscribers=500 \
 *     --perf_rounds=5 --perf_round_duration_ms=10000 \
 *     --perf_top_n=5 --perf_output=perf_results.json \
 *     --perf_event_log=topn_events.log
 */

#include "relay/SimpleTopNTracker.h"

#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <folly/portability/GFlags.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <moxygen/test/MockMoQSession.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <mach/mach.h>
#elif defined(__linux__)
#include <fstream>
#endif

using namespace testing;
using namespace moxygen;
using namespace openmoq::moqx;
using namespace std::chrono;

DEFINE_int32(perf_panelists, 50, "Number of panelists (pub-subscribers)");
DEFINE_int32(perf_subscribers, 500, "Number of pure subscribers");
DEFINE_int32(perf_top_n, 5, "Top-N value for selection");
DEFINE_int32(perf_rounds, 5, "Number of test rounds");
DEFINE_int32(perf_round_duration_ms, 10000, "Duration per round in ms");
DEFINE_int32(perf_group_interval_ms, 20, "Group interval (property update rate)");
DEFINE_int32(perf_reader_threads, 4, "Number of concurrent reader threads");
DEFINE_string(perf_output, "", "JSON output file path (empty = stdout)");
DEFINE_string(perf_event_log, "", "TOPN_EVENT log file path (empty = disabled)");
DEFINE_bool(perf_verbose, false, "Verbose output during test");

namespace {

const TrackNamespace kNs{{"webinar", "audio"}};

FullTrackName ftn(const std::string& name) {
  return FullTrackName{kNs, name};
}

std::string trackNameForPanelist(int id) {
  return "panelist-" + std::to_string(id);
}

// --- Memory measurement ---

size_t getCurrentRssBytes() {
#ifdef __APPLE__
  struct mach_task_basic_info info;
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS) {
    return info.resident_size;
  }
  return 0;
#elif defined(__linux__)
  std::ifstream statm("/proc/self/statm");
  size_t pages = 0;
  statm >> pages; // Total program size (ignored)
  statm >> pages; // Resident set size in pages
  return pages * sysconf(_SC_PAGESIZE);
#else
  return 0;
#endif
}

// --- Speech State Machine (matching moq-rs speech.rs) ---

enum class SpeechState { Silent, SpeechStart, Speaking, SpeechEnded };

class SpeechSimulator {
public:
  explicit SpeechSimulator(std::mt19937& rng) : rng_(rng) {
    silenceDuration_ = randomSilenceDuration();
    stateStart_ = steady_clock::now();
  }

  uint8_t tick() {
    auto elapsed = steady_clock::now() - stateStart_;

    switch (state_) {
    case SpeechState::Silent:
      if (elapsed >= silenceDuration_) {
        state_ = SpeechState::SpeechStart;
        stateStart_ = steady_clock::now();
        speechDuration_ = randomSpeechDuration();
        sentFinalZero_ = false;
      }
      break;

    case SpeechState::SpeechStart:
      if (elapsed >= kSpeechStartDuration) {
        state_ = SpeechState::Speaking;
      }
      if (steady_clock::now() - stateStart_ >= speechDuration_) {
        state_ = SpeechState::SpeechEnded;
        stateStart_ = steady_clock::now();
      }
      break;

    case SpeechState::Speaking:
      if (elapsed >= speechDuration_ - kSpeechStartDuration) {
        state_ = SpeechState::SpeechEnded;
        stateStart_ = steady_clock::now();
      }
      break;

    case SpeechState::SpeechEnded:
      if (!sentFinalZero_) {
        sentFinalZero_ = true;
      } else {
        state_ = SpeechState::Silent;
        stateStart_ = steady_clock::now();
        silenceDuration_ = randomSilenceDuration();
      }
      break;
    }

    return currentValue();
  }

  uint8_t currentValue() const {
    switch (state_) {
    case SpeechState::Silent:
      return 0;
    case SpeechState::SpeechStart:
      return 2;
    case SpeechState::Speaking:
      return 1;
    case SpeechState::SpeechEnded:
      return sentFinalZero_ ? 0 : 1;
    }
    return 0;
  }

  SpeechState state() const { return state_; }

private:
  static constexpr auto kSpeechStartDuration = milliseconds{300};

  milliseconds randomSpeechDuration() {
    std::uniform_int_distribution<int> dist(2000, 8000);
    return milliseconds{dist(rng_)};
  }

  milliseconds randomSilenceDuration() {
    std::uniform_int_distribution<int> dist(1000, 5000);
    return milliseconds{dist(rng_)};
  }

  std::mt19937& rng_;
  SpeechState state_{SpeechState::Silent};
  steady_clock::time_point stateStart_{steady_clock::now()};
  milliseconds speechDuration_{0};
  milliseconds silenceDuration_{0};
  bool sentFinalZero_{true};
};

// --- Latency histogram for percentile computation ---

class LatencyHistogram {
public:
  void record(uint64_t valueNs) {
    std::lock_guard<std::mutex> lock(mu_);
    samples_.push_back(valueNs);
  }

  struct Stats {
    uint64_t count{0};
    uint64_t min{0};
    uint64_t max{0};
    uint64_t mean{0};
    uint64_t p50{0};
    uint64_t p95{0};
    uint64_t p99{0};
    uint64_t total{0};
  };

  Stats compute() {
    std::lock_guard<std::mutex> lock(mu_);
    Stats s;
    if (samples_.empty()) return s;

    std::sort(samples_.begin(), samples_.end());
    s.count = samples_.size();
    s.min = samples_.front();
    s.max = samples_.back();
    s.total = std::accumulate(samples_.begin(), samples_.end(), uint64_t{0});
    s.mean = s.total / s.count;
    s.p50 = samples_[s.count * 50 / 100];
    s.p95 = samples_[s.count * 95 / 100];
    s.p99 = samples_[std::min(s.count - 1, s.count * 99 / 100)];
    return s;
  }

  void reset() {
    std::lock_guard<std::mutex> lock(mu_);
    samples_.clear();
  }

private:
  std::mutex mu_;
  std::vector<uint64_t> samples_;
};

// --- TOPN_EVENT Logger ---

class TopNEventLogger {
public:
  explicit TopNEventLogger(const std::string& path) {
    if (!path.empty()) {
      file_.open(path);
      enabled_ = file_.is_open();
    }
  }

  bool enabled() const { return enabled_; }

  void logTrackRegistered(uint64_t tsMs, const std::string& track, uint64_t value, uint64_t pubId) {
    if (!enabled_) return;
    std::lock_guard<std::mutex> lock(mu_);
    file_ << "TOPN_EVENT:{\"event\":\"track_registered\",\"ts_ms\":" << tsMs
           << ",\"track\":\"" << track << "\",\"value\":" << value
           << ",\"publisher_id\":" << pubId << "}\n";
  }

  void logValueUpdated(
      uint64_t tsMs,
      const std::string& track,
      uint64_t oldValue,
      uint64_t newValue,
      uint64_t pubId
  ) {
    if (!enabled_) return;
    std::lock_guard<std::mutex> lock(mu_);
    file_ << "TOPN_EVENT:{\"event\":\"value_updated\",\"ts_ms\":" << tsMs
           << ",\"track\":\"" << track << "\",\"old_value\":" << oldValue
           << ",\"new_value\":" << newValue << ",\"publisher_id\":" << pubId << "}\n";
  }

  void logSubscriberRegistered(uint64_t tsMs, uint64_t subId, bool isPubSub, uint64_t pubId) {
    if (!enabled_) return;
    std::lock_guard<std::mutex> lock(mu_);
    file_ << "TOPN_EVENT:{\"event\":\"subscriber_registered\",\"ts_ms\":" << tsMs
           << ",\"subscriber_id\":" << subId << ",\"is_pub_sub\":" << (isPubSub ? "true" : "false")
           << ",\"publisher_id\":" << pubId << "}\n";
  }

  void logTopNQuery(
      uint64_t tsMs,
      uint64_t subId,
      uint8_t n,
      const std::vector<std::pair<std::string, uint64_t>>& selected,
      uint64_t excludedSelf
  ) {
    if (!enabled_) return;
    std::lock_guard<std::mutex> lock(mu_);
    file_ << "TOPN_EVENT:{\"event\":\"top_n_query\",\"ts_ms\":" << tsMs
           << ",\"subscriber_id\":" << subId << ",\"n\":" << static_cast<int>(n) << ",\"selected\":[";
    for (size_t i = 0; i < selected.size(); ++i) {
      if (i > 0) file_ << ",";
      file_ << "{\"track\":\"" << selected[i].first << "\",\"value\":" << selected[i].second << "}";
    }
    file_ << "]";
    if (excludedSelf > 0) {
      file_ << ",\"excluded_self\":" << excludedSelf;
    }
    file_ << "}\n";
  }

  void flush() {
    if (enabled_) file_.flush();
  }

private:
  bool enabled_{false};
  std::mutex mu_;
  std::ofstream file_;
};

// --- Per-round metrics ---

struct RoundMetrics {
  int round{0};

  // Throughput
  uint64_t updatesPerformed{0};
  uint64_t snapshotRebuilds{0};
  uint64_t snapshotReads{0};
  uint64_t topNQueries{0};
  uint64_t selectCallbacks{0};
  uint64_t evictCallbacks{0};

  // Latency (nanoseconds)
  LatencyHistogram::Stats updateLatency;
  LatencyHistogram::Stats queryLatency;
  LatencyHistogram::Stats rebuildLatency;
  LatencyHistogram::Stats callbackLatency;

  // Fast rejection
  uint64_t fastRejections{0};
  uint64_t fullScans{0};

  // Memory
  size_t rssBeforeBytes{0};
  size_t rssAfterBytes{0};
  size_t snapshotBytes{0};
  size_t sessionStateBytes{0};

  // Correctness
  uint64_t selfExclusionViolations{0};

  // Speech stats
  uint64_t speechStartEvents{0};
  uint64_t silentUpdates{0};
  uint64_t speakingUpdates{0};

  double durationSec{0};
};

// --- JSON output ---

std::string metricsToJson(const std::vector<RoundMetrics>& rounds) {
  std::ostringstream os;
  os << "{\n  \"test\": \"SimpleTopNTracker_PerfTest\",\n";
  os << "  \"config\": {\n";
  os << "    \"panelists\": " << FLAGS_perf_panelists << ",\n";
  os << "    \"subscribers\": " << FLAGS_perf_subscribers << ",\n";
  os << "    \"top_n\": " << FLAGS_perf_top_n << ",\n";
  os << "    \"rounds\": " << FLAGS_perf_rounds << ",\n";
  os << "    \"round_duration_ms\": " << FLAGS_perf_round_duration_ms << ",\n";
  os << "    \"group_interval_ms\": " << FLAGS_perf_group_interval_ms << ",\n";
  os << "    \"reader_threads\": " << FLAGS_perf_reader_threads << "\n";
  os << "  },\n";
  os << "  \"rounds\": [\n";

  for (size_t i = 0; i < rounds.size(); ++i) {
    const auto& r = rounds[i];
    os << "    {\n";
    os << "      \"round\": " << r.round << ",\n";
    os << "      \"duration_sec\": " << std::fixed << std::setprecision(3) << r.durationSec << ",\n";

    os << "      \"throughput\": {\n";
    os << "        \"updates\": " << r.updatesPerformed << ",\n";
    os << "        \"snapshot_rebuilds\": " << r.snapshotRebuilds << ",\n";
    os << "        \"snapshot_reads\": " << r.snapshotReads << ",\n";
    os << "        \"top_n_queries\": " << r.topNQueries << ",\n";
    os << "        \"select_callbacks\": " << r.selectCallbacks << ",\n";
    os << "        \"evict_callbacks\": " << r.evictCallbacks << ",\n";
    os << "        \"updates_per_sec\": " << std::fixed << std::setprecision(1)
       << (r.updatesPerformed / r.durationSec) << ",\n";
    os << "        \"queries_per_sec\": " << std::fixed << std::setprecision(1)
       << (r.topNQueries / r.durationSec) << "\n";
    os << "      },\n";

    auto writeLatency = [&](const char* name, const LatencyHistogram::Stats& s) {
      os << "      \"" << name << "\": {\n";
      os << "        \"count\": " << s.count << ",\n";
      os << "        \"min_ns\": " << s.min << ",\n";
      os << "        \"max_ns\": " << s.max << ",\n";
      os << "        \"mean_ns\": " << s.mean << ",\n";
      os << "        \"p50_ns\": " << s.p50 << ",\n";
      os << "        \"p95_ns\": " << s.p95 << ",\n";
      os << "        \"p99_ns\": " << s.p99 << ",\n";
      os << "        \"total_ns\": " << s.total << "\n";
      os << "      },\n";
    };

    writeLatency("update_latency", r.updateLatency);
    writeLatency("query_latency", r.queryLatency);
    writeLatency("rebuild_latency", r.rebuildLatency);
    writeLatency("callback_latency", r.callbackLatency);

    os << "      \"fast_rejection\": {\n";
    uint64_t total = r.fastRejections + r.fullScans;
    double rate = total > 0 ? (100.0 * r.fastRejections / total) : 0;
    os << "        \"fast_rejections\": " << r.fastRejections << ",\n";
    os << "        \"full_scans\": " << r.fullScans << ",\n";
    os << "        \"rejection_rate_pct\": " << std::fixed << std::setprecision(1) << rate << "\n";
    os << "      },\n";

    os << "      \"memory\": {\n";
    os << "        \"rss_before_bytes\": " << r.rssBeforeBytes << ",\n";
    os << "        \"rss_after_bytes\": " << r.rssAfterBytes << ",\n";
    os << "        \"rss_delta_bytes\": " << (r.rssAfterBytes > r.rssBeforeBytes ? r.rssAfterBytes - r.rssBeforeBytes : 0) << ",\n";
    os << "        \"snapshot_bytes\": " << r.snapshotBytes << ",\n";
    os << "        \"session_state_bytes\": " << r.sessionStateBytes << "\n";
    os << "      },\n";

    os << "      \"speech\": {\n";
    os << "        \"speech_start_events\": " << r.speechStartEvents << ",\n";
    os << "        \"silent_updates\": " << r.silentUpdates << ",\n";
    os << "        \"speaking_updates\": " << r.speakingUpdates << "\n";
    os << "      },\n";

    os << "      \"correctness\": {\n";
    os << "        \"self_exclusion_violations\": " << r.selfExclusionViolations << "\n";
    os << "      }\n";

    os << "    }" << (i + 1 < rounds.size() ? "," : "") << "\n";
  }

  os << "  ]\n}\n";
  return os.str();
}

// --- Test fixture ---

class SimpleTopNTrackerPerfTest : public ::testing::Test {
protected:
  void SetUp() override {
    for (int i = 0; i < FLAGS_perf_panelists; ++i) {
      auto session = std::make_shared<NiceMock<moxygen::test::MockMoQSession>>();
      panelistSessions_.push_back(session);
    }
    for (int i = 0; i < FLAGS_perf_subscribers; ++i) {
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
};

TEST_F(SimpleTopNTrackerPerfTest, MultiRoundWebinarSimulation) {
  TopNEventLogger eventLog(FLAGS_perf_event_log);
  auto baseTs = steady_clock::now();
  auto msFromBase = [&]() -> uint64_t {
    return duration_cast<milliseconds>(steady_clock::now() - baseTs).count();
  };

  std::cout << "\n";
  std::cout << "================================================================================\n";
  std::cout << "         SIMPLE N+X TOP-N PERFORMANCE TEST — WEBINAR SIMULATION\n";
  std::cout << "================================================================================\n\n";
  std::cout << "Configuration:\n";
  std::cout << "  Panelists (pub-sub):  " << FLAGS_perf_panelists << "\n";
  std::cout << "  Pure Subscribers:     " << FLAGS_perf_subscribers << "\n";
  std::cout << "  Top-N:                " << FLAGS_perf_top_n << "\n";
  std::cout << "  Rounds:               " << FLAGS_perf_rounds << "\n";
  std::cout << "  Round Duration:       " << FLAGS_perf_round_duration_ms << " ms\n";
  std::cout << "  Group Interval:       " << FLAGS_perf_group_interval_ms << " ms\n";
  std::cout << "  Reader Threads:       " << FLAGS_perf_reader_threads << "\n";
  if (eventLog.enabled()) {
    std::cout << "  Event Log:            " << FLAGS_perf_event_log << "\n";
  }
  std::cout << "\n";

  // Shared counters for callback tracking
  std::atomic<uint64_t> selectCallbacks{0};
  std::atomic<uint64_t> evictCallbacks{0};
  LatencyHistogram callbackLatencyHist;

  // Create tracker with callback instrumentation
  SimpleTopNTracker tracker(
      /*propertyType=*/1,
      /*idleTimeout=*/milliseconds{0},
      /*sweepThrottle=*/milliseconds{0},
      /*getLastActivity=*/[](const FullTrackName&) { return steady_clock::now(); },
      /*onSnapshotChanged=*/
      [&](auto oldSnap, auto newSnap, auto removed) {
        auto start = steady_clock::now();
        // Simulate what SimpleTopNRanking does: recompute for all sessions
        // This measures the callback cost that the relay pays
        for (int i = 0; i < std::min(FLAGS_perf_panelists, 10); ++i) {
          auto session = getPanelistSession(i);
          auto topN = tracker.computeTopNForSession(session, FLAGS_perf_top_n, *newSnap);
          (void)topN;
        }
        auto elapsed = duration_cast<nanoseconds>(steady_clock::now() - start).count();
        callbackLatencyHist.record(elapsed);
      }
  );

  // Phase 1: Register sessions
  std::cout << "Phase 1: Registering sessions...\n";

  for (int i = 0; i < FLAGS_perf_panelists; ++i) {
    tracker.addSession(FLAGS_perf_top_n, getPanelistSession(i), true);
    if (eventLog.enabled()) {
      eventLog.logSubscriberRegistered(msFromBase(), i, true, i);
    }
  }
  for (int i = 0; i < FLAGS_perf_subscribers; ++i) {
    tracker.addSession(FLAGS_perf_top_n, getSubscriberSession(i), true);
    if (eventLog.enabled()) {
      eventLog.logSubscriberRegistered(
          msFromBase(), FLAGS_perf_panelists + i, false, 0);
    }
  }

  // Phase 2: Register tracks
  std::cout << "Phase 2: Registering tracks (one per panelist)...\n";

  for (int i = 0; i < FLAGS_perf_panelists; ++i) {
    tracker.registerTrack(ftn(trackNameForPanelist(i)), 0, getPanelistSession(i));
    if (eventLog.enabled()) {
      eventLog.logTrackRegistered(msFromBase(), trackNameForPanelist(i), 0, i);
    }
  }

  // Phase 3: Run rounds
  std::vector<RoundMetrics> allRounds;

  for (int round = 0; round < FLAGS_perf_rounds; ++round) {
    std::cout << "\n--- Round " << (round + 1) << "/" << FLAGS_perf_rounds << " ---\n";

    RoundMetrics metrics;
    metrics.round = round + 1;
    metrics.rssBeforeBytes = getCurrentRssBytes();

    LatencyHistogram updateHist;
    LatencyHistogram queryHist;
    LatencyHistogram rebuildHist;
    callbackLatencyHist.reset();

    std::atomic<uint64_t> roundRebuilds{0};
    std::atomic<uint64_t> roundReads{0};
    std::atomic<uint64_t> roundQueries{0};
    std::atomic<uint64_t> roundFastRejections{0};
    std::atomic<uint64_t> roundFullScans{0};
    std::atomic<uint64_t> roundSelfViolations{0};
    std::atomic<uint64_t> roundSpeechStarts{0};
    std::atomic<uint64_t> roundSilent{0};
    std::atomic<uint64_t> roundSpeaking{0};

    selectCallbacks.store(0);
    evictCallbacks.store(0);

    std::atomic<bool> running{true};
    auto roundStart = steady_clock::now();

    // Update thread: speech simulation for all panelists
    std::thread updateThread([&]() {
      std::vector<std::mt19937> rngs;
      std::vector<SpeechSimulator> speakers;
      for (int i = 0; i < FLAGS_perf_panelists; ++i) {
        rngs.emplace_back(round * 1000 + i);
        speakers.emplace_back(rngs.back());
      }

      // Track previous values for event logging
      std::vector<uint8_t> prevValues(FLAGS_perf_panelists, 0);

      auto groupInterval = milliseconds{FLAGS_perf_group_interval_ms};
      auto nextTick = steady_clock::now();

      while (running.load(std::memory_order_relaxed)) {
        auto now = steady_clock::now();
        if (now < nextTick) {
          std::this_thread::sleep_for(nextTick - now);
        }
        nextTick += groupInterval;

        for (int i = 0; i < FLAGS_perf_panelists; ++i) {
          uint8_t value = speakers[i].tick();

          if (value == 2) roundSpeechStarts++;
          else if (value == 0) roundSilent++;
          else roundSpeaking++;

          auto start = steady_clock::now();
          tracker.updateSortValue(ftn(trackNameForPanelist(i)), value);
          auto elapsed = duration_cast<nanoseconds>(steady_clock::now() - start).count();
          updateHist.record(elapsed);
          rebuildHist.record(elapsed);
          roundRebuilds++;

          if (eventLog.enabled() && value != prevValues[i]) {
            eventLog.logValueUpdated(msFromBase(), trackNameForPanelist(i), prevValues[i], value, i);
            prevValues[i] = value;
          }
        }

        metrics.updatesPerformed += FLAGS_perf_panelists;
      }
    });

    // Reader threads: concurrent top-N queries
    std::vector<std::thread> readerThreads;
    for (int r = 0; r < FLAGS_perf_reader_threads; ++r) {
      readerThreads.emplace_back([&, r]() {
        std::mt19937 rng(round * 10000 + r);
        std::uniform_int_distribution<int> panelistDist(0, FLAGS_perf_panelists - 1);
        std::uniform_int_distribution<int> subDist(0, FLAGS_perf_subscribers - 1);
        std::uniform_int_distribution<int> typeDist(0, 9);

        while (running.load(std::memory_order_relaxed)) {
          // Lock-free snapshot read
          auto readStart = steady_clock::now();
          auto snapshot = tracker.loadSnapshot();
          roundReads++;

          if (!snapshot || snapshot->empty()) {
            std::this_thread::yield();
            continue;
          }

          // 70% queries for pure subscribers, 30% for panelists (pub-subs)
          int queryType = typeDist(rng);
          std::shared_ptr<MoQSession> session;
          int sessionId;
          bool isPubSub;

          if (queryType < 7) {
            sessionId = subDist(rng);
            session = getSubscriberSession(sessionId);
            isPubSub = false;
          } else {
            sessionId = panelistDist(rng);
            session = getPanelistSession(sessionId);
            isPubSub = true;
          }

          // Top-N query with self-exclusion
          auto queryStart = steady_clock::now();
          auto topN = tracker.computeTopNForSession(session, FLAGS_perf_top_n, *snapshot);
          auto queryElapsed = duration_cast<nanoseconds>(steady_clock::now() - queryStart).count();
          queryHist.record(queryElapsed);
          roundQueries++;

          // Correctness: verify self-exclusion for panelists
          if (isPubSub) {
            std::string selfTrack = trackNameForPanelist(sessionId);
            for (const auto& track : topN) {
              if (track.trackName == selfTrack) {
                roundSelfViolations++;
              }
            }
          }

          // Fast rejection measurement
          if (isPubSub && !snapshot->empty()) {
            uint8_t lastSelfPos = tracker.computeLastSelfPosition(session, *snapshot);
            for (size_t pos = 0; pos < snapshot->size(); ++pos) {
              if (SimpleTopNTracker::mightBeInTopN(pos, FLAGS_perf_top_n, lastSelfPos)) {
                roundFullScans++;
              } else {
                roundFastRejections++;
              }
            }
          }

          // Log top-N query events (sampled: every 50th query to avoid log bloat)
          if (eventLog.enabled() && (roundQueries.load() % 50) == 0) {
            std::vector<std::pair<std::string, uint64_t>> selected;
            for (const auto& t : topN) {
              // Find value in snapshot
              uint64_t val = 0;
              for (const auto& s : *snapshot) {
                if (s.ftn == t) {
                  val = s.propertyValue;
                  break;
                }
              }
              selected.emplace_back(t.trackName, val);
            }
            eventLog.logTopNQuery(
                msFromBase(),
                isPubSub ? sessionId : FLAGS_perf_panelists + sessionId,
                FLAGS_perf_top_n,
                selected,
                isPubSub ? sessionId : 0
            );
          }

          std::this_thread::yield();
        }
      });
    }

    // Wait for round duration
    std::this_thread::sleep_for(milliseconds(FLAGS_perf_round_duration_ms));
    running.store(false);

    updateThread.join();
    for (auto& t : readerThreads) {
      t.join();
    }

    auto roundEnd = steady_clock::now();
    metrics.durationSec = duration_cast<microseconds>(roundEnd - roundStart).count() / 1e6;
    metrics.rssAfterBytes = getCurrentRssBytes();

    // Collect metrics
    metrics.snapshotRebuilds = roundRebuilds.load();
    metrics.snapshotReads = roundReads.load();
    metrics.topNQueries = roundQueries.load();
    metrics.selectCallbacks = selectCallbacks.load();
    metrics.evictCallbacks = evictCallbacks.load();
    metrics.fastRejections = roundFastRejections.load();
    metrics.fullScans = roundFullScans.load();
    metrics.selfExclusionViolations = roundSelfViolations.load();
    metrics.speechStartEvents = roundSpeechStarts.load();
    metrics.silentUpdates = roundSilent.load();
    metrics.speakingUpdates = roundSpeaking.load();

    metrics.updateLatency = updateHist.compute();
    metrics.queryLatency = queryHist.compute();
    metrics.rebuildLatency = rebuildHist.compute();
    metrics.callbackLatency = callbackLatencyHist.compute();

    // Estimate memory usage
    auto snapshot = tracker.loadSnapshot();
    metrics.snapshotBytes = snapshot ? snapshot->size() * sizeof(TrackRank) : 0;
    metrics.sessionStateBytes =
        (FLAGS_perf_panelists + FLAGS_perf_subscribers) * sizeof(SessionSelectionState);

    // Print round summary
    std::cout << "  Duration:      " << std::fixed << std::setprecision(2) << metrics.durationSec
              << "s\n";
    std::cout << "  Updates:       " << metrics.updatesPerformed << " ("
              << std::fixed << std::setprecision(0) << (metrics.updatesPerformed / metrics.durationSec)
              << "/s)\n";
    std::cout << "  Queries:       " << metrics.topNQueries << " ("
              << std::fixed << std::setprecision(0) << (metrics.topNQueries / metrics.durationSec)
              << "/s)\n";
    std::cout << "  Update p50:    " << (metrics.updateLatency.p50 / 1000) << " us, p99: "
              << (metrics.updateLatency.p99 / 1000) << " us\n";
    std::cout << "  Query p50:     " << (metrics.queryLatency.p50 / 1000) << " us, p99: "
              << (metrics.queryLatency.p99 / 1000) << " us\n";
    std::cout << "  Rejection:     " << std::fixed << std::setprecision(1)
              << (metrics.fastRejections + metrics.fullScans > 0
                      ? 100.0 * metrics.fastRejections / (metrics.fastRejections + metrics.fullScans)
                      : 0)
              << "%\n";
    std::cout << "  RSS delta:     "
              << ((int64_t)metrics.rssAfterBytes - (int64_t)metrics.rssBeforeBytes) / 1024
              << " KB\n";
    std::cout << "  Violations:    " << metrics.selfExclusionViolations << "\n";

    allRounds.push_back(metrics);
  }

  // Final report
  std::cout << "\n";
  std::cout << "================================================================================\n";
  std::cout << "                         AGGREGATE PERFORMANCE REPORT\n";
  std::cout << "================================================================================\n\n";

  // Compute aggregates
  uint64_t totalUpdates = 0, totalQueries = 0;
  double totalDuration = 0;
  uint64_t totalUpdateP50 = 0, totalQueryP50 = 0;
  uint64_t totalUpdateP99 = 0, totalQueryP99 = 0;

  for (const auto& r : allRounds) {
    totalUpdates += r.updatesPerformed;
    totalQueries += r.topNQueries;
    totalDuration += r.durationSec;
    totalUpdateP50 += r.updateLatency.p50;
    totalQueryP50 += r.queryLatency.p50;
    totalUpdateP99 += r.updateLatency.p99;
    totalQueryP99 += r.queryLatency.p99;
  }

  size_t numRounds = allRounds.size();
  std::cout << "Avg Update Rate:     " << std::fixed << std::setprecision(0)
            << (totalUpdates / totalDuration) << " updates/sec\n";
  std::cout << "Avg Query Rate:      " << std::fixed << std::setprecision(0)
            << (totalQueries / totalDuration) << " queries/sec\n";
  std::cout << "Avg Update p50:      " << (totalUpdateP50 / numRounds / 1000) << " us\n";
  std::cout << "Avg Update p99:      " << (totalUpdateP99 / numRounds / 1000) << " us\n";
  std::cout << "Avg Query p50:       " << (totalQueryP50 / numRounds / 1000) << " us\n";
  std::cout << "Avg Query p99:       " << (totalQueryP99 / numRounds / 1000) << " us\n";

  auto finalSnapshot = tracker.loadSnapshot();
  std::cout << "\nSnapshot Size:       " << (finalSnapshot ? finalSnapshot->size() : 0)
            << " tracks (" << (finalSnapshot ? finalSnapshot->size() * sizeof(TrackRank) : 0)
            << " bytes)\n";
  std::cout << "Session State:       "
            << (FLAGS_perf_panelists + FLAGS_perf_subscribers) * sizeof(SessionSelectionState)
            << " bytes (for " << (FLAGS_perf_panelists + FLAGS_perf_subscribers) << " sessions)\n";

  uint64_t totalViolations = 0;
  for (const auto& r : allRounds) totalViolations += r.selfExclusionViolations;
  std::cout << "\nTotal Violations:    " << totalViolations << "\n";
  std::cout << "Status:              " << (totalViolations == 0 ? "PASSED" : "FAILED") << "\n\n";

  // Write JSON output
  std::string json = metricsToJson(allRounds);
  if (!FLAGS_perf_output.empty()) {
    std::ofstream out(FLAGS_perf_output);
    out << json;
    std::cout << "Results written to: " << FLAGS_perf_output << "\n";
  } else if (FLAGS_perf_verbose) {
    std::cout << "\nJSON Output:\n" << json;
  }

  if (eventLog.enabled()) {
    eventLog.flush();
    std::cout << "Event log written to: " << FLAGS_perf_event_log << "\n";
  }

  std::cout << "================================================================================\n\n";

  EXPECT_EQ(totalViolations, 0) << "Self-exclusion violations detected across rounds";
}

TEST_F(SimpleTopNTrackerPerfTest, BaselineComparison) {
  // Measures the cost of top-N filtering vs pure forwarding (no filtering).
  // This shows the marginal overhead of the Simple N+X algorithm.

  std::cout << "\n";
  std::cout << "================================================================================\n";
  std::cout << "         BASELINE COMPARISON: Top-N Overhead vs No Filtering\n";
  std::cout << "================================================================================\n\n";

  const int numObjects = 100000;

  // Baseline: no filtering (just iterate and "forward" all)
  auto baselineStart = steady_clock::now();
  volatile uint64_t sink = 0;
  for (int i = 0; i < numObjects; ++i) {
    sink += i;
  }
  auto baselineDuration = duration_cast<nanoseconds>(steady_clock::now() - baselineStart);

  // With top-N: create tracker, register tracks, then evaluate
  SimpleTopNTracker tracker(
      1, milliseconds{0}, milliseconds{0},
      [](const FullTrackName&) { return steady_clock::now(); },
      [](auto, auto, auto) {}
  );

  for (int i = 0; i < FLAGS_perf_panelists; ++i) {
    tracker.addSession(FLAGS_perf_top_n, getPanelistSession(i), true);
    tracker.registerTrack(ftn(trackNameForPanelist(i)), (i % 3) + 1, getPanelistSession(i));
  }

  auto snapshot = tracker.loadSnapshot();
  auto session = getPanelistSession(0);
  uint8_t lastSelfPos = tracker.computeLastSelfPosition(session, *snapshot);

  // Measure: per-object filter evaluation
  auto filterStart = steady_clock::now();
  uint64_t forwarded = 0;
  for (int obj = 0; obj < numObjects; ++obj) {
    int trackIdx = obj % FLAGS_perf_panelists;
    auto& trackFtn = (*snapshot)[trackIdx % snapshot->size()].ftn;
    if (tracker.isInTopNWithSelfExclusion(trackFtn, session, FLAGS_perf_top_n, *snapshot, lastSelfPos)) {
      forwarded++;
    }
  }
  auto filterDuration = duration_cast<nanoseconds>(steady_clock::now() - filterStart);

  double baselinePerObj = static_cast<double>(baselineDuration.count()) / numObjects;
  double filterPerObj = static_cast<double>(filterDuration.count()) / numObjects;
  double overhead = filterPerObj - baselinePerObj;
  double overheadPct = baselinePerObj > 0 ? (overhead / baselinePerObj * 100) : 0;

  std::cout << "Objects evaluated:   " << numObjects << "\n";
  std::cout << "Baseline per-obj:    " << std::fixed << std::setprecision(1) << baselinePerObj
            << " ns\n";
  std::cout << "Filter per-obj:      " << std::fixed << std::setprecision(1) << filterPerObj
            << " ns\n";
  std::cout << "Overhead per-obj:    " << std::fixed << std::setprecision(1) << overhead
            << " ns (" << std::fixed << std::setprecision(0) << overheadPct << "% over baseline)\n";
  std::cout << "Forwarded:           " << forwarded << "/" << numObjects << "\n";
  std::cout << "\n================================================================================\n\n";

  EXPECT_GT(forwarded, 0u);
}

} // namespace

int main(int argc, char* argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  return RUN_ALL_TESTS();
}

