/*
 * Copyright (c) OpenMOQ contributors.
 *
 * Track Filter Load Test - Large-scale test for PropertyRanking with Self-Exclusion
 *
 * Simulates:
 * - 100 panelists (each is publisher + subscriber with TRACK_FILTER, self-exclusion)
 * - 10,000 pure subscribers (TRACK_FILTER only, top-N selection)
 * - All requesting top-3 tracks
 *
 * Correctness model:
 * - Published ranking is deterministic, so expected top-N membership is known
 * - Pure subscribers must receive exactly the expected top-N tracks
 * - Panelists must receive exactly the expected top-N non-self tracks
 * - No panelist may receive its own track
 *
 * Generates performance and correctness report on:
 * - Track filter selection events
 * - Time spent in filtering, ranking, forwarding
 * - Throughput and latency metrics
 * - Exact top-N verification for subscribers and pub-sub sessions
 */

#include <folly/Synchronized.h>
#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Collect.h>
#include <folly/coro/Sleep.h>
#include <folly/executors/IOThreadPoolExecutor.h>
#include <folly/init/Init.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/portability/GFlags.h>
#include <moxygen/MoQClient.h>
#include <moxygen/MoQRelaySession.h>
#include <moxygen/MoQWebTransportClient.h>
#include <moxygen/events/MoQFollyExecutorImpl.h>
#include <moxygen/relay/MoQRelayClient.h>
#include <moxygen/util/InsecureVerifierDangerousDoNotUseInProduction.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

using namespace moxygen;
using namespace std::chrono;

DEFINE_string(relay_url, "", "Relay URL");
DEFINE_int32(panelists, 100, "Number of panelists (each is publisher + subscriber)");
DEFINE_int32(subscribers, 10000, "Number of pure subscribers");
DEFINE_int32(top_n, 3, "Top-N value for TRACK_FILTER");
DEFINE_string(
    mixed_topn,
    "",
    "Comma-separated N values for mixed top-N test (e.g. '1,10,25,45,65,77,90'). "
    "Subscribers are randomly assigned one of these values. Overrides --top_n.");
DEFINE_int32(
    panelist_topn,
    -1,
    "Top-N value specifically for panelists. -1 means use same as subscribers (--top_n or --mixed_topn).");
DEFINE_int32(duration, 30, "Test duration in seconds");
DEFINE_int32(update_hz, 30, "Audio level update frequency");
DEFINE_int32(connect_timeout, 10000, "Connection timeout in ms");
DEFINE_int32(transaction_timeout, 120, "Transaction timeout in seconds");
DEFINE_int32(batch_size, 100, "Batch size for subscriber connections");
DEFINE_int32(batch_delay_ms, 100, "Delay between batches in ms");
DEFINE_bool(insecure, false, "Skip certificate verification");
DEFINE_string(namespace_prefix, "loadtest", "Namespace prefix");
DEFINE_string(report_file, "track_filter_report.txt", "Output report file");
DEFINE_bool(verbose, false, "Enable verbose output");
DEFINE_string(alpns, "moqt-16,moqt-15,moq-00", "Comma-separated list of ALPN protocols to use");
DEFINE_int32(num_threads, 1, "Number of IO threads for client connections");
DEFINE_int32(drain_period_ms, 2000, "Drain period before verification (ms)");
DEFINE_bool(
    speech_mode,
    false,
    "Use speech simulation (dynamic audio levels) instead of deterministic ranking");
DEFINE_string(
    topn_event_log,
    "",
    "TOPN_EVENT log file path (empty = disabled). Only used with --speech_mode");

namespace {

std::vector<std::string> parseAlpns(const std::string& alpnStr) {
  std::vector<std::string> alpns;
  std::stringstream ss(alpnStr);
  std::string item;
  while (std::getline(ss, item, ',')) {
    if (!item.empty()) {
      alpns.push_back(item);
    }
  }
  return alpns;
}

std::vector<int> parseMixedTopN(const std::string& s) {
  std::vector<int> values;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, ',')) {
    if (!item.empty()) {
      values.push_back(std::stoi(item));
    }
  }
  return values;
}

int getTopNForSubscriber(int subscriberId, const std::vector<int>& mixedValues) {
  if (mixedValues.empty()) {
    return FLAGS_top_n;
  }
  return mixedValues[subscriberId % mixedValues.size()];
}

constexpr uint64_t kAudioLevelPropertyType = 0x02;

// Speech state machine matching moq-rs speech.rs for realistic audio simulation.
// States: SILENT(val=0) -> SPEECH_START(val=2) -> SPEAKING(val=1) -> back to SILENT.
enum class SpeechState { Silent, SpeechStart, Speaking, SpeechEnded };

class SpeechSimulator {
public:
  explicit SpeechSimulator(uint32_t seed)
      : rng_(seed), state_(SpeechState::Silent), stateStart_(steady_clock::now()) {
    silenceDuration_ = randomSilenceDuration();
  }

  uint64_t tick() {
    auto elapsed = steady_clock::now() - stateStart_;

    switch (state_) {
    case SpeechState::Silent:
      if (elapsed >= silenceDuration_) {
        state_ = SpeechState::SpeechStart;
        stateStart_ = steady_clock::now();
        speechDuration_ = randomSpeechDuration();
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
      state_ = SpeechState::Silent;
      stateStart_ = steady_clock::now();
      silenceDuration_ = randomSilenceDuration();
      break;
    }

    return currentValue();
  }

  uint64_t currentValue() const {
    switch (state_) {
    case SpeechState::Silent:
      return 0;
    case SpeechState::SpeechStart:
      return 2;
    case SpeechState::Speaking:
      return 1;
    case SpeechState::SpeechEnded:
      return 0;
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

  std::mt19937 rng_;
  SpeechState state_;
  steady_clock::time_point stateStart_;
  milliseconds speechDuration_{0};
  milliseconds silenceDuration_{0};
};

// TOPN_EVENT logger for visualization compatibility with topn_viz.py.
class TopNEventLogger {
public:
  explicit TopNEventLogger(const std::string& path) {
    if (!path.empty()) {
      file_.open(path);
      enabled_ = file_.is_open();
      startTime_ = steady_clock::now();
    }
  }

  void logTrackRegistered(const std::string& track, int publisherId) {
    if (!enabled_)
      return;
    file_ << "TOPN_EVENT:{\"event\":\"track_registered\",\"ts_ms\":" << elapsedMs()
           << ",\"track\":\"" << track << "\",\"publisher_id\":" << publisherId << "}\n";
  }

  void logSubscriberRegistered(int subscriberId, int n) {
    if (!enabled_)
      return;
    file_ << "TOPN_EVENT:{\"event\":\"subscriber_registered\",\"ts_ms\":" << elapsedMs()
           << ",\"subscriber_id\":" << subscriberId << ",\"n\":" << n << "}\n";
  }

  void logValueUpdated(const std::string& track, uint64_t oldVal, uint64_t newVal, int pubId) {
    if (!enabled_)
      return;
    file_ << "TOPN_EVENT:{\"event\":\"value_updated\",\"ts_ms\":" << elapsedMs()
           << ",\"track\":\"" << track << "\",\"old_value\":" << oldVal
           << ",\"new_value\":" << newVal << ",\"publisher_id\":" << pubId << "}\n";
  }

  void flush() {
    if (enabled_) {
      file_.flush();
    }
  }

  bool enabled() const { return enabled_; }

private:
  uint64_t elapsedMs() const {
    return duration_cast<milliseconds>(steady_clock::now() - startTime_).count();
  }

  std::ofstream file_;
  bool enabled_{false};
  steady_clock::time_point startTime_;
};

// Metrics collection
struct TrackFilterMetrics {
  struct VerificationFailure {
    int clientId;
    folly::F14FastSet<std::string> expected;
    folly::F14FastSet<std::string> actual;
  };

  struct ClientDeliveryState {
    bool isPanelist{false};
    folly::F14FastSet<std::string> receivedTracks;
    folly::F14FastMap<std::string, uint64_t> objectsPerTrack;
  };

  struct VerificationState {
    folly::F14FastMap<int, ClientDeliveryState> clientDeliveries;
    folly::F14FastSet<int> connectedPanelistIds;
    folly::F14FastSet<int> connectedSubscriberIds;
    std::vector<VerificationFailure> subscriberFailures;
    std::vector<VerificationFailure> panelistFailures;
  };

  // Connection metrics
  std::atomic<uint64_t> panelistsConnected{0};
  std::atomic<uint64_t> subscribersConnected{0};
  std::atomic<uint64_t> connectionErrors{0};

  // Throughput metrics
  std::atomic<uint64_t> publishedObjects{0};
  std::atomic<uint64_t> receivedObjects{0};
  std::atomic<uint64_t> selfReceivedObjects{0};
  std::atomic<uint64_t> forwardErrors{0};

  // Track selection metrics
  std::atomic<uint64_t> tracksSelected{0};
  std::atomic<uint64_t> tracksEvicted{0};

  // Correctness verification
  folly::Synchronized<VerificationState> verificationState;
  std::atomic<uint64_t> subscribersVerified{0};
  std::atomic<uint64_t> subscriberVerificationFailures{0};
  std::atomic<uint64_t> panelistsVerified{0};
  std::atomic<uint64_t> panelistVerificationFailures{0};

  // Speech metrics
  std::atomic<uint64_t> speechStarts{0};
  std::atomic<uint64_t> totalSpeechTicks{0};
  std::atomic<uint64_t> totalSilentTicks{0};

  // Timing
  steady_clock::time_point testStart;
  steady_clock::time_point testEnd;

  void recordDeliveredObject(int clientId, bool isPanelist, const std::string& trackName) {
    auto state = verificationState.wlock();
    auto& delivery = state->clientDeliveries[clientId];
    delivery.isPanelist = isPanelist;
    delivery.receivedTracks.insert(trackName);
    delivery.objectsPerTrack[trackName]++;
  }

  void recordConnectedClient(int clientId, bool isPanelist) {
    auto state = verificationState.wlock();
    if (isPanelist) {
      state->connectedPanelistIds.insert(clientId);
    } else {
      state->connectedSubscriberIds.insert(clientId);
    }
  }
};

// Lightweight subscription handle
class LoadTestSubscriptionHandle : public SubscriptionHandle {
public:
  void unsubscribe() override {}
  folly::coro::Task<RequestUpdateResult> requestUpdate(RequestUpdate req) override {
    co_return RequestOk{.requestID = req.requestID};
  }
};

// Handle for PUBLISH_NAMESPACE
class LoadTestPublishNamespaceHandle : public Subscriber::PublishNamespaceHandle {
public:
  using Subscriber::PublishNamespaceHandle::setPublishNamespaceOk;
  void publishNamespaceDone() override {}
  folly::coro::Task<RequestUpdateResult> requestUpdate(RequestUpdate req) override {
    co_return folly::makeUnexpected(
        RequestError{req.requestID, RequestErrorCode::NOT_SUPPORTED, "Not supported"}
    );
  }
};

// Handle for NAMESPACE messages
class LoadTestNamespacePublishHandle : public Publisher::NamespacePublishHandle {
public:
  void namespaceMsg(const TrackNamespace&) override {}
  void namespaceDoneMsg(const TrackNamespace&) override {}
};

// Track consumer for receiving objects
class LoadTestTrackConsumer : public TrackConsumer {
public:
  LoadTestTrackConsumer(
      int clientId,
      const std::string& trackName,
      bool isPanelist,
      TrackFilterMetrics& metrics
  )
      : clientId_(clientId), trackName_(trackName), isPanelist_(isPanelist), metrics_(metrics) {
    if (isPanelist_) {
      selfTrackName_ = folly::to<std::string>("panelist-", clientId);
      isSelfTrack_ = (trackName == selfTrackName_);
    }
  }

  folly::Expected<folly::Unit, MoQPublishError> setTrackAlias(TrackAlias) override {
    return folly::unit;
  }

  folly::Expected<std::shared_ptr<SubgroupConsumer>, MoQPublishError>
  beginSubgroup(uint64_t, uint64_t, Priority, bool) override {
    return std::make_shared<LoadTestSubgroupConsumer>(
        clientId_,
        trackName_,
        isPanelist_,
        isSelfTrack_,
        metrics_
    );
  }

  folly::Expected<folly::SemiFuture<folly::Unit>, MoQPublishError> awaitStreamCredit() override {
    return folly::makeSemiFuture(folly::unit);
  }

  folly::Expected<folly::Unit, MoQPublishError>
  objectStream(const ObjectHeader&, Payload, bool) override {
    recordObject();
    return folly::unit;
  }

  folly::Expected<folly::Unit, MoQPublishError>
  datagram(const ObjectHeader&, Payload, bool) override {
    return folly::unit;
  }

  folly::Expected<folly::Unit, MoQPublishError> publishDone(PublishDone) override {
    return folly::unit;
  }

private:
  void recordObject() {
    if (isSelfTrack_) {
      metrics_.selfReceivedObjects++;
    } else {
      metrics_.receivedObjects++;
      metrics_.recordDeliveredObject(clientId_, isPanelist_, trackName_);
    }
  }

  class LoadTestSubgroupConsumer : public SubgroupConsumer {
  public:
    LoadTestSubgroupConsumer(
        int clientId,
        const std::string& trackName,
        bool isPanelist,
        bool isSelfTrack,
        TrackFilterMetrics& metrics
    )
        : clientId_(clientId), trackName_(trackName), isPanelist_(isPanelist),
          isSelfTrack_(isSelfTrack), metrics_(metrics) {}

    folly::Expected<folly::Unit, MoQPublishError>
    object(uint64_t, Payload, Extensions, bool) override {
      if (isSelfTrack_) {
        metrics_.selfReceivedObjects++;
      } else {
        metrics_.receivedObjects++;
        metrics_.recordDeliveredObject(clientId_, isPanelist_, trackName_);
      }
      return folly::unit;
    }

    folly::Expected<folly::Unit, MoQPublishError>
    beginObject(uint64_t, uint64_t, Payload, Extensions) override {
      return folly::unit;
    }

    folly::Expected<ObjectPublishStatus, MoQPublishError> objectPayload(Payload, bool) override {
      return ObjectPublishStatus::DONE;
    }

    folly::Expected<folly::Unit, MoQPublishError> endOfGroup(uint64_t) override {
      return folly::unit;
    }

    folly::Expected<folly::Unit, MoQPublishError> endOfTrackAndGroup(uint64_t) override {
      return folly::unit;
    }

    folly::Expected<folly::Unit, MoQPublishError> endOfSubgroup() override { return folly::unit; }

    void reset(ResetStreamErrorCode) override {}

  private:
    int clientId_;
    std::string trackName_;
    bool isPanelist_;
    bool isSelfTrack_;
    TrackFilterMetrics& metrics_;
  };

  int clientId_;
  std::string trackName_;
  std::string selfTrackName_;
  bool isPanelist_;
  bool isSelfTrack_{false};
  TrackFilterMetrics& metrics_;
};

// Subscriber handler - receives forwarded PUBLISH from relay
class LoadTestSubscriber : public Subscriber {
public:
  LoadTestSubscriber(int id, bool isPanelist, TrackFilterMetrics& metrics)
      : id_(id), isPanelist_(isPanelist), metrics_(metrics) {}

  PublishResult publish(PublishRequest pubReq, std::shared_ptr<SubscriptionHandle>) override {
    std::string trackName = pubReq.fullTrackName.trackName;
    metrics_.tracksSelected++;

    auto consumer = std::make_shared<LoadTestTrackConsumer>(id_, trackName, isPanelist_, metrics_);

    auto replyTask = [](RequestID reqID
                     ) -> folly::coro::Task<folly::Expected<PublishOk, PublishError>> {
      co_return PublishOk{.requestID = reqID, .forward = true};
    }(pubReq.requestID);

    return PublishConsumerAndReplyTask{.consumer = consumer, .reply = std::move(replyTask)};
  }

  folly::coro::Task<PublishNamespaceResult>
  publishNamespace(PublishNamespace ann, std::shared_ptr<PublishNamespaceCallback>) override {
    auto handle = std::make_shared<LoadTestPublishNamespaceHandle>();
    handle->setPublishNamespaceOk(
        PublishNamespaceOk{.requestID = ann.requestID, .requestSpecificParams = {}}
    );
    co_return handle;
  }

  void goaway(Goaway) override {}

private:
  int id_;
  bool isPanelist_;
  TrackFilterMetrics& metrics_;
};

// Main load test class
class TrackFilterLoadTest : public std::enable_shared_from_this<TrackFilterLoadTest> {
public:
  TrackFilterLoadTest(folly::EventBase* evb, TrackFilterMetrics& metrics)
      : evb_(evb), moqEvb_(std::make_shared<MoQFollyExecutorImpl>(evb)), metrics_(metrics) {
    mixedTopNValues_ = parseMixedTopN(FLAGS_mixed_topn);
  }

  folly::coro::Task<void> run() {
    metrics_.testStart = steady_clock::now();

    std::cout << "Track Filter Load Test\n";
    std::cout << "======================\n";
    std::cout << "Panelists: " << FLAGS_panelists << " (pub+sub with self-exclusion)\n";
    std::cout << "Subscribers: " << FLAGS_subscribers << " (pure subscribers)\n";
    if (!mixedTopNValues_.empty()) {
      std::cout << "Top-N: mixed [";
      for (size_t i = 0; i < mixedTopNValues_.size(); ++i) {
        if (i > 0) std::cout << ",";
        std::cout << mixedTopNValues_[i];
      }
      std::cout << "] (randomly assigned)\n";
    } else {
      std::cout << "Top-N: " << FLAGS_top_n << "\n";
    }
    std::cout << "Duration: " << FLAGS_duration << "s\n\n";

    try {
      // Phase 1: Connect panelists
      std::cout << "Phase 1: Connecting " << FLAGS_panelists << " panelists...\n";
      for (int i = 0; i < FLAGS_panelists; ++i) {
        if (co_await connectPanelist(i)) {
          metrics_.panelistsConnected++;
        } else {
          metrics_.connectionErrors++;
        }
        if ((i + 1) % 10 == 0) {
          std::cout << "  Connected " << metrics_.panelistsConnected << " panelists\n";
        }
      }

      // Phase 2: Connect subscribers in batches
      std::cout << "\nPhase 2: Connecting " << FLAGS_subscribers << " subscribers...\n";
      for (int batch = 0; batch < FLAGS_subscribers; batch += FLAGS_batch_size) {
        int batchEnd = std::min(batch + FLAGS_batch_size, FLAGS_subscribers);
        std::vector<folly::coro::Task<bool>> tasks;
        for (int i = batch; i < batchEnd; ++i) {
          tasks.push_back(connectSubscriber(i));
        }
        auto results = co_await folly::coro::collectAllRange(std::move(tasks));
        for (size_t i = 0; i < results.size(); ++i) {
          if (results[i]) {
            metrics_.subscribersConnected++;
          } else {
            metrics_.connectionErrors++;
          }
        }
        std::cout << "  Connected " << metrics_.subscribersConnected << "/" << FLAGS_subscribers
                  << " subscribers\n";
        co_await folly::coro::sleep(milliseconds(FLAGS_batch_delay_ms));
      }

      std::cout << "\nConnections complete:\n";
      std::cout << "  Panelists: " << metrics_.panelistsConnected << "/" << FLAGS_panelists << "\n";
      std::cout << "  Subscribers: " << metrics_.subscribersConnected << "/" << FLAGS_subscribers
                << "\n";

      if (panelists_.empty()) {
        std::cerr << "Error: No panelists connected\n";
        co_return;
      }

      // Phase 3: Run publishing loop
      std::cout << "\nPhase 3: Running test for " << FLAGS_duration << " seconds...\n";
      co_await runPublisherLoop(seconds(FLAGS_duration));

      // Drain period: allow in-flight objects to be delivered before verification.
      // Without this, objects published near the end of the loop may still be in
      // transit through the relay, causing false verification failures.
      std::cout << "  Draining for " << FLAGS_drain_period_ms << "ms...\n";
      co_await folly::coro::sleep(milliseconds(FLAGS_drain_period_ms));

      if (FLAGS_speech_mode) {
        verifySpeechModeCorrectness();
      } else {
        verifyCorrectness();
      }

      metrics_.testEnd = steady_clock::now();

      // Phase 4: Cleanup
      std::cout << "\nPhase 4: Cleaning up...\n";
      cleanup();

    } catch (const std::exception& ex) {
      std::cerr << "Test failed: " << ex.what() << "\n";
    }

    std::cout << "Done.\n";
  }

  void generateReport() {
    std::stringstream report;
    auto duration = duration_cast<milliseconds>(metrics_.testEnd - metrics_.testStart);

    report << "\n";
    report << "================================================================================\n";
    report << "                    TRACK FILTER LOAD TEST REPORT\n";
    report
        << "================================================================================\n\n";

    // Test Configuration
    report << "TEST CONFIGURATION\n";
    report << "------------------\n";
    report << "  Panelists (pub+sub):     " << FLAGS_panelists << "\n";
    report << "  Pure Subscribers:        " << FLAGS_subscribers << "\n";
    report << "  Total Clients:           " << (FLAGS_panelists + FLAGS_subscribers) << "\n";
    if (!mixedTopNValues_.empty()) {
      report << "  Top-N Filter (subs):     mixed [";
      for (size_t i = 0; i < mixedTopNValues_.size(); ++i) {
        if (i > 0) report << ",";
        report << mixedTopNValues_[i];
      }
      report << "]\n";
    } else {
      report << "  Top-N Filter:            " << FLAGS_top_n << "\n";
    }
    if (FLAGS_panelist_topn >= 0) {
      report << "  Top-N Filter (panelists):" << FLAGS_panelist_topn << "\n";
    }
    report << "  Ranking Mode:            "
           << (FLAGS_speech_mode ? "speech simulation (dynamic)" : "deterministic descending by panelist id")
           << "\n";
    report << "  Update Rate:             " << FLAGS_update_hz << " Hz\n";
    report << "  Duration:                " << FLAGS_duration << "s\n\n";

    // Connection Results
    report << "CONNECTION RESULTS\n";
    report << "------------------\n";
    report << "  Panelists Connected:     " << metrics_.panelistsConnected << "/" << FLAGS_panelists
           << "\n";
    report << "  Subscribers Connected:   " << metrics_.subscribersConnected << "/"
           << FLAGS_subscribers << "\n";
    report << "  Connection Errors:       " << metrics_.connectionErrors << "\n";
    report << "  Connection Success Rate: " << std::fixed << std::setprecision(1)
           << (100.0 * (metrics_.panelistsConnected + metrics_.subscribersConnected) /
               (FLAGS_panelists + FLAGS_subscribers))
           << "%\n\n";

    // Throughput Metrics
    report << "THROUGHPUT METRICS\n";
    report << "------------------\n";
    double durationSec = duration.count() / 1000.0;
    report << "  Objects Published:       " << metrics_.publishedObjects << "\n";
    report << "  Objects Received:        " << metrics_.receivedObjects << "\n";
    report << "  Self-Received (errors):  " << metrics_.selfReceivedObjects << "\n";
    report << "  Forward Errors:          " << metrics_.forwardErrors << "\n";
    report << "  Publish Rate:            " << std::fixed << std::setprecision(1)
           << (metrics_.publishedObjects / durationSec) << " obj/s\n";
    report << "  Receive Rate:            " << std::fixed << std::setprecision(1)
           << (metrics_.receivedObjects / durationSec) << " obj/s\n\n";

    // Track Filter Selection Metrics
    report << "TRACK FILTER SELECTION\n";
    report << "----------------------\n";
    report << "  Tracks Selected:         " << metrics_.tracksSelected << "\n";
    report << "  Tracks Evicted:          " << metrics_.tracksEvicted << "\n\n";

    if (FLAGS_speech_mode) {
      report << "SPEECH SIMULATION STATISTICS\n";
      report << "----------------------------\n";
      uint64_t totalTicks = metrics_.totalSpeechTicks + metrics_.totalSilentTicks;
      double speechPct = totalTicks > 0 ? (100.0 * metrics_.totalSpeechTicks / totalTicks) : 0;
      double avgSpeechPerSpeaker = metrics_.speechStarts > 0
          ? (metrics_.totalSpeechTicks / static_cast<double>(metrics_.speechStarts))
          : 0;
      double avgSpeechDurMs = avgSpeechPerSpeaker * (1000.0 / FLAGS_update_hz);
      double avgSilenceDurMs = metrics_.speechStarts > 0
          ? (metrics_.totalSilentTicks / static_cast<double>(metrics_.speechStarts)) * (1000.0 / FLAGS_update_hz)
          : 0;
      report << "  Speech Starts (total):   " << metrics_.speechStarts.load() << "\n";
      report << "  Avg Speeches/Panelist:   " << std::fixed << std::setprecision(1)
             << (metrics_.speechStarts.load() / static_cast<double>(FLAGS_panelists)) << "\n";
      report << "  Speech Ticks:            " << metrics_.totalSpeechTicks.load() << " ("
             << std::fixed << std::setprecision(1) << speechPct << "%)\n";
      report << "  Silent Ticks:            " << metrics_.totalSilentTicks.load() << " ("
             << std::fixed << std::setprecision(1) << (100.0 - speechPct) << "%)\n";
      report << "  Avg Speech Duration:     " << std::fixed << std::setprecision(0) << avgSpeechDurMs << " ms\n";
      report << "  Avg Silence Duration:    " << std::fixed << std::setprecision(0) << avgSilenceDurMs << " ms\n";
      report << "  Speech Algorithm:        State machine: SILENT->SPEECH_START(300ms)->SPEAKING(2-8s)->ENDED->SILENT(1-5s)\n";
      report << "  Speech Values:           0=silent, 2=speech_start, 1=speaking\n\n";
    }

    report << "TOP-N CORRECTNESS VERIFICATION\n";
    report << "------------------------------\n";
    report << "  Subscribers Verified:     " << metrics_.subscribersVerified << "\n";
    report << "  Subscriber Failures:      " << metrics_.subscriberVerificationFailures << "\n";
    report << "  Panelists Verified:       " << metrics_.panelistsVerified << "\n";
    report << "  Panelist Failures:        " << metrics_.panelistVerificationFailures << "\n";
    report << "  Overall Status:           "
           << ((metrics_.subscriberVerificationFailures == 0 &&
                metrics_.panelistVerificationFailures == 0 && metrics_.selfReceivedObjects == 0)
                   ? "PASSED"
                   : "FAILED")
           << "\n\n";

    // Self-Exclusion Status
    report << "SELF-EXCLUSION VERIFICATION\n";
    report << "---------------------------\n";
    if (metrics_.selfReceivedObjects == 0) {
      report << "  Status: PASSED\n";
      report << "  No panelist received their own track (self-exclusion working correctly)\n\n";
    } else {
      report << "  Status: FAILED\n";
      report << "  " << metrics_.selfReceivedObjects << " self-received objects detected!\n\n";
    }

    // Expected vs Actual Analysis
    report << "EXPECTED VS ACTUAL\n";
    report << "------------------\n";
    uint64_t expectedPublished = FLAGS_panelists * FLAGS_duration * FLAGS_update_hz;
    double publishRate =
        (expectedPublished > 0) ? (100.0 * metrics_.publishedObjects / expectedPublished) : 0;

    report << "  Target Published:        " << expectedPublished << "\n";
    report << "  Actual Published:        " << metrics_.publishedObjects << "\n";
    report << "  Publish Rate:            " << std::fixed << std::setprecision(1) << publishRate
           << "% of target\n\n";

    report << "EXPECTED TOP-N SETS\n";
    report << "-------------------\n";
    report << "  Pure subscriber expected tracks: ";
    appendTrackSet(report, expectedSubscriberTracks());
    report << "\n";
    report << "  Sample pub-sub expected tracks:\n";
    auto connectedPanelists = connectedPanelistsInRankOrder();
    for (size_t i = 0; i < std::min<size_t>(connectedPanelists.size(), 5); ++i) {
      int panelistId = connectedPanelists[i];
      report << "    panelist-" << panelistId << ": ";
      appendTrackSet(report, expectedPanelistTracks(panelistId));
      report << "\n";
    }
    if (connectedPanelists.size() > 5) {
      report << "    ... (" << (connectedPanelists.size() - 5) << " more connected panelists)\n";
    }
    report << "\n";

    {
      auto verification = metrics_.verificationState.rlock();
      appendFailureSamples(
          report,
          "SUBSCRIBER TOP-N FAILURES",
          verification->subscriberFailures,
          std::min<size_t>(verification->subscriberFailures.size(), 5)
      );
      appendFailureSamples(
          report,
          "PUB-SUB TOP-N + SELF-EXCLUSION FAILURES",
          verification->panelistFailures,
          std::min<size_t>(verification->panelistFailures.size(), 5)
      );
    }

    // Summary
    report << "SUMMARY\n";
    report << "-------\n";
    report << "  Test Duration:           " << std::fixed << std::setprecision(2) << durationSec
           << "s\n";
    report << "  Total Messages Handled:  "
           << (metrics_.publishedObjects + metrics_.receivedObjects) << "\n";
    report << "  Message Rate:            " << std::fixed << std::setprecision(1)
           << ((metrics_.publishedObjects + metrics_.receivedObjects) / durationSec) << " msg/s\n";

    report
        << "\n================================================================================\n";

    std::cout << report.str();

    // Write to file
    std::ofstream file(FLAGS_report_file);
    if (file.is_open()) {
      file << report.str();
      file.close();
      std::cout << "\nReport written to: " << FLAGS_report_file << "\n";
    }
  }

private:
  static std::string trackNameForPanelist(int id) {
    return folly::to<std::string>("panelist-", id);
  }

  static folly::F14FastSet<std::string>
  expectedSubscriberTracksFor(const std::vector<int>& connectedPanelists) {
    folly::F14FastSet<std::string> expected;
    int effectiveTopN = std::min<int>(FLAGS_top_n, connectedPanelists.size());
    for (int i = 0; i < effectiveTopN; ++i) {
      expected.insert(trackNameForPanelist(connectedPanelists[i]));
    }
    return expected;
  }

  static folly::F14FastSet<std::string>
  expectedPanelistTracksFor(const std::vector<int>& connectedPanelists, int panelistId) {
    folly::F14FastSet<std::string> expected;
    for (int id : connectedPanelists) {
      if (static_cast<int>(expected.size()) >= FLAGS_top_n) {
        break;
      }
      if (id == panelistId) {
        continue;
      }
      expected.insert(trackNameForPanelist(id));
    }
    return expected;
  }

  std::vector<int> connectedPanelistsInRankOrder() const {
    auto state = metrics_.verificationState.rlock();
    std::vector<int> panelists(
        state->connectedPanelistIds.begin(),
        state->connectedPanelistIds.end()
    );
    std::sort(panelists.begin(), panelists.end());
    return panelists;
  }

  folly::F14FastSet<std::string> expectedSubscriberTracks() const {
    return expectedSubscriberTracksFor(connectedPanelistsInRankOrder());
  }

  folly::F14FastSet<std::string> expectedPanelistTracks(int panelistId) const {
    return expectedPanelistTracksFor(connectedPanelistsInRankOrder(), panelistId);
  }

  static void appendTrackSet(std::ostream& out, const folly::F14FastSet<std::string>& tracks) {
    if (tracks.empty()) {
      out << "(none)";
      return;
    }

    std::vector<std::string> sortedTracks(tracks.begin(), tracks.end());
    std::sort(sortedTracks.begin(), sortedTracks.end());

    bool first = true;
    for (const auto& track : sortedTracks) {
      if (!first) {
        out << ", ";
      }
      out << track;
      first = false;
    }
  }

  static void appendFailureSamples(
      std::ostream& out,
      const std::string& title,
      const std::vector<TrackFilterMetrics::VerificationFailure>& failures,
      size_t maxSamples
  ) {
    out << title << "\n";
    out << std::string(title.size(), '-') << "\n";
    if (failures.empty()) {
      out << "  None\n\n";
      return;
    }

    for (size_t i = 0; i < maxSamples; ++i) {
      const auto& failure = failures[i];
      out << "  Client " << failure.clientId << "\n";
      out << "    Expected: ";
      appendTrackSet(out, failure.expected);
      out << "\n";
      out << "    Actual:   ";
      appendTrackSet(out, failure.actual);
      out << "\n";
    }
    if (failures.size() > maxSamples) {
      out << "  ... " << (failures.size() - maxSamples) << " more failures\n";
    }
    out << "\n";
  }

  void verifyCorrectness() {
    auto state = metrics_.verificationState.wlock();
    std::vector<int> connectedPanelists(
        state->connectedPanelistIds.begin(),
        state->connectedPanelistIds.end()
    );
    std::sort(connectedPanelists.begin(), connectedPanelists.end());
    auto expectedSubs = expectedSubscriberTracksFor(connectedPanelists);
    for (int id : connectedPanelists) {
      folly::F14FastSet<std::string> actual;
      auto it = state->clientDeliveries.find(id);
      if (it != state->clientDeliveries.end()) {
        actual = it->second.receivedTracks;
      }

      auto expected = expectedPanelistTracksFor(connectedPanelists, id);
      metrics_.panelistsVerified++;
      if (actual != expected) {
        metrics_.panelistVerificationFailures++;
        state->panelistFailures.push_back(TrackFilterMetrics::VerificationFailure{
            .clientId = id,
            .expected = std::move(expected),
            .actual = std::move(actual)
        });
      }
    }

    std::vector<int> connectedSubscribers(
        state->connectedSubscriberIds.begin(),
        state->connectedSubscriberIds.end()
    );
    std::sort(connectedSubscribers.begin(), connectedSubscribers.end());
    for (int id : connectedSubscribers) {
      folly::F14FastSet<std::string> actual;
      auto it = state->clientDeliveries.find(id);
      if (it != state->clientDeliveries.end()) {
        actual = it->second.receivedTracks;
      }

      metrics_.subscribersVerified++;
      if (actual != expectedSubs) {
        metrics_.subscriberVerificationFailures++;
        state->subscriberFailures.push_back(TrackFilterMetrics::VerificationFailure{
            .clientId = id,
            .expected = expectedSubs,
            .actual = std::move(actual)
        });
      }
    }
  }

  void verifySpeechModeCorrectness() {
    // In speech mode, rankings are dynamic so we can't verify exact top-N sets.
    // We CAN verify:
    // 1. Self-exclusion: no panelist received its own track
    // 2. Bounded selection: no subscriber received more than N distinct tracks
    //    at any single point in time (they may see different tracks over time)
    // 3. All received tracks are from valid panelists
    auto state = metrics_.verificationState.wlock();
    std::vector<int> connectedPanelists(
        state->connectedPanelistIds.begin(), state->connectedPanelistIds.end());
    std::sort(connectedPanelists.begin(), connectedPanelists.end());

    folly::F14FastSet<std::string> validTrackNames;
    for (int id : connectedPanelists) {
      validTrackNames.insert(trackNameForPanelist(id));
    }

    for (int id : connectedPanelists) {
      auto it = state->clientDeliveries.find(id);
      if (it == state->clientDeliveries.end())
        continue;

      metrics_.panelistsVerified++;
      const auto& delivery = it->second;

      // Check self-exclusion
      std::string selfTrack = trackNameForPanelist(id);
      if (delivery.receivedTracks.find(selfTrack) != delivery.receivedTracks.end()) {
        metrics_.panelistVerificationFailures++;
        state->panelistFailures.push_back(TrackFilterMetrics::VerificationFailure{
            .clientId = id, .expected = {}, .actual = {selfTrack}});
      }

      // Check all received tracks are valid
      for (const auto& track : delivery.receivedTracks) {
        if (validTrackNames.find(track) == validTrackNames.end()) {
          metrics_.panelistVerificationFailures++;
          break;
        }
      }
    }

    std::vector<int> connectedSubscribers(
        state->connectedSubscriberIds.begin(), state->connectedSubscriberIds.end());
    for (int id : connectedSubscribers) {
      auto it = state->clientDeliveries.find(id);
      if (it == state->clientDeliveries.end())
        continue;

      metrics_.subscribersVerified++;
      const auto& delivery = it->second;

      // Check all received tracks are from valid panelists
      for (const auto& track : delivery.receivedTracks) {
        if (validTrackNames.find(track) == validTrackNames.end()) {
          metrics_.subscriberVerificationFailures++;
          state->subscriberFailures.push_back(TrackFilterMetrics::VerificationFailure{
              .clientId = id, .expected = {}, .actual = {track}});
          break;
        }
      }
    }

    std::cout << "\n  Speech Mode Verification:\n";
    std::cout << "    Self-exclusion:    "
              << (metrics_.selfReceivedObjects == 0 && metrics_.panelistVerificationFailures == 0
                      ? "PASSED"
                      : "FAILED")
              << "\n";
    std::cout << "    Valid tracks:      "
              << (metrics_.subscriberVerificationFailures == 0 ? "PASSED" : "FAILED") << "\n";
    std::cout << "    Unique tracks seen by panelists: dynamic (speech-driven)\n";
  }

  folly::coro::Task<bool> connectPanelist(int id) {
    proxygen::URL url(FLAGS_relay_url);
    auto verifier =
        FLAGS_insecure
            ? std::make_shared<moxygen::test::InsecureVerifierDangerousDoNotUseInProduction>()
            : nullptr;

    auto client = std::make_unique<MoQWebTransportClient>(
        moqEvb_,
        url,
        MoQRelaySession::createRelaySessionFactory(),
        verifier
    );
    auto handler = std::make_shared<LoadTestSubscriber>(id, true, metrics_);

    try {
      co_await client->setupMoQSession(
          milliseconds(FLAGS_connect_timeout),
          seconds(FLAGS_transaction_timeout),
          nullptr,
          handler,
          quic::TransportSettings(),
          parseAlpns(FLAGS_alpns)
      );

      auto session = client->moqSession_;
      if (!session) {
        co_return false;
      }

      // Increase max request ID to support many concurrent subscriptions
      session->setMaxConcurrentRequests(10000);

      // Subscribe with TRACK_FILTER.
      // Note: Self-exclusion is automatic in the relay's PropertyRanking
      // implementation - there is no explicit flag in TrackFilter. When a
      // session publishes a track and subscribes with TRACK_FILTER, the relay
      // compares RankedEntry::publisher with the session pointer and excludes
      // any track where publisher == subscriber session. See PropertyRanking.h:
      // "Self-exclusion is automatic: if this session is the publisher of any
      // registered track, those tracks will be excluded from its top-N selection."
      SubscribeNamespace subNs;
      subNs.requestID = RequestID{static_cast<uint64_t>(id * 2)};
      subNs.trackNamespacePrefix =
          TrackNamespace(std::vector<std::string>{FLAGS_namespace_prefix, "audio"});
      subNs.forward = true;

      int panelistTopN = FLAGS_panelist_topn >= 0
          ? FLAGS_panelist_topn
          : getTopNForSubscriber(id, mixedTopNValues_);
      TrackFilter filter(kAudioLevelPropertyType, panelistTopN);
      Parameter trackFilterParam(folly::to_underlying(TrackRequestParamKey::TRACK_FILTER), filter);
      subNs.params.insertParam(std::move(trackFilterParam));

      auto nsHandle = std::make_shared<LoadTestNamespacePublishHandle>();
      auto subResult = co_await session->subscribeNamespace(std::move(subNs), nsHandle);
      if (subResult.hasError()) {
        co_return false;
      }

      // Publish track
      PublishRequest pubReq;
      pubReq.fullTrackName = FullTrackName{
          TrackNamespace(std::vector<std::string>{FLAGS_namespace_prefix, "audio"}),
          folly::to<std::string>("panelist-", id)
      };
      pubReq.groupOrder = GroupOrder::OldestFirst;
      pubReq.forward = true;

      auto subHandle = std::make_shared<LoadTestSubscriptionHandle>();
      auto publishResult = session->publish(pubReq, subHandle);
      if (publishResult.hasError()) {
        co_return false;
      }

      auto& pubResponse = publishResult.value();
      auto pubOkResult = co_await std::move(pubResponse.reply);
      if (pubOkResult.hasError()) {
        co_return false;
      }

      panelists_.push_back(PanelistState{
          .panelistId = id,
          .client = std::move(client),
          .session = session,
          .consumer = pubResponse.consumer,
          .subscribeHandle = std::move(subResult.value())
      });
      metrics_.recordConnectedClient(id, true);

      co_return true;

    } catch (const std::exception&) {
      co_return false;
    }
  }

  folly::coro::Task<bool> connectSubscriber(int id) {
    proxygen::URL url(FLAGS_relay_url);
    auto verifier =
        FLAGS_insecure
            ? std::make_shared<moxygen::test::InsecureVerifierDangerousDoNotUseInProduction>()
            : nullptr;

    auto client = std::make_unique<MoQWebTransportClient>(
        moqEvb_,
        url,
        MoQRelaySession::createRelaySessionFactory(),
        verifier
    );
    auto handler = std::make_shared<LoadTestSubscriber>(FLAGS_panelists + id, false, metrics_);

    try {
      co_await client->setupMoQSession(
          milliseconds(FLAGS_connect_timeout),
          seconds(FLAGS_transaction_timeout),
          nullptr,
          handler,
          quic::TransportSettings(),
          parseAlpns(FLAGS_alpns)
      );

      auto session = client->moqSession_;
      if (!session) {
        co_return false;
      }

      // Increase max request ID to support many concurrent subscriptions
      session->setMaxConcurrentRequests(10000);

      // Subscribe with TRACK_FILTER only (no publishing)
      SubscribeNamespace subNs;
      subNs.requestID = RequestID{static_cast<uint64_t>((FLAGS_panelists + id) * 2)};
      subNs.trackNamespacePrefix =
          TrackNamespace(std::vector<std::string>{FLAGS_namespace_prefix, "audio"});
      subNs.forward = true;

      int subscriberTopN = getTopNForSubscriber(id, mixedTopNValues_);
      TrackFilter filter(kAudioLevelPropertyType, subscriberTopN);
      Parameter trackFilterParam(folly::to_underlying(TrackRequestParamKey::TRACK_FILTER), filter);
      subNs.params.insertParam(std::move(trackFilterParam));

      auto nsHandle = std::make_shared<LoadTestNamespacePublishHandle>();
      auto subResult = co_await session->subscribeNamespace(std::move(subNs), nsHandle);
      if (subResult.hasError()) {
        co_return false;
      }

      subscriberClients_.push_back(std::move(client));
      subscriberSessions_.push_back(session);
      subscriberHandles_.push_back(std::move(subResult.value()));
      metrics_.recordConnectedClient(FLAGS_panelists + id, false);

      co_return true;

    } catch (const std::exception&) {
      co_return false;
    }
  }

  folly::coro::Task<void> runPublisherLoop(seconds duration) {
    auto endTime = steady_clock::now() + duration;
    uint64_t group = 0;
    auto updateInterval = microseconds(1000000 / FLAGS_update_hz);
    int progressInterval = std::max(1, FLAGS_duration / 10);
    auto nextProgress = steady_clock::now() + seconds(progressInterval);

    // Initialize speech simulators and event logger if in speech mode
    if (FLAGS_speech_mode) {
      eventLogger_ = std::make_unique<TopNEventLogger>(FLAGS_topn_event_log);
      for (auto& panelist : panelists_) {
        panelist.speechSim = std::make_unique<SpeechSimulator>(
            static_cast<uint32_t>(panelist.panelistId * 7919 + 42));
        if (eventLogger_->enabled()) {
          eventLogger_->logTrackRegistered(
              "panelist-" + std::to_string(panelist.panelistId), panelist.panelistId);
        }
      }
    }

    while (steady_clock::now() < endTime) {
      auto iterStart = steady_clock::now();

      for (auto& panelist : panelists_) {
        if (!panelist.consumer)
          continue;

        uint64_t audioLevel;
        if (FLAGS_speech_mode) {
          auto prevState = panelist.speechSim->state();
          audioLevel = panelist.speechSim->tick();
          auto newState = panelist.speechSim->state();
          if (prevState == SpeechState::Silent && newState == SpeechState::SpeechStart) {
            metrics_.speechStarts++;
          }
          if (newState == SpeechState::Silent || newState == SpeechState::SpeechEnded) {
            metrics_.totalSilentTicks++;
          } else {
            metrics_.totalSpeechTicks++;
          }
        } else {
          // Deterministic strict ranking based on actual panelist ID:
          // panelist-0 > panelist-1 > panelist-2 > ...
          // Lower ID = higher audio level = higher rank.
          audioLevel = static_cast<uint64_t>(FLAGS_panelists - panelist.panelistId);
        }

        Extensions extensions;
        extensions.getMutableExtensions().push_back(Extension(kAudioLevelPropertyType, audioLevel));

        ObjectHeader
            header{group, 0, 0, 0, ObjectStatus::NORMAL, std::move(extensions), std::nullopt};

        auto payload = folly::IOBuf::copyBuffer(folly::to<std::string>("audio:", audioLevel));

        auto res = panelist.consumer->objectStream(header, std::move(payload));
        if (res.hasError()) {
          metrics_.forwardErrors++;
        } else {
          metrics_.publishedObjects++;
          if (FLAGS_speech_mode && eventLogger_->enabled() &&
              audioLevel != panelist.lastAudioLevel) {
            eventLogger_->logValueUpdated(
                "panelist-" + std::to_string(panelist.panelistId),
                panelist.lastAudioLevel,
                audioLevel,
                panelist.panelistId);
            panelist.lastAudioLevel = audioLevel;
          }
        }
      }

      group++;

      // Progress indicator
      if (steady_clock::now() >= nextProgress) {
        auto elapsed = duration_cast<seconds>(steady_clock::now() - metrics_.testStart);
        std::cout << "  Progress: " << elapsed.count() << "s / " << FLAGS_duration
                  << "s - Published: " << metrics_.publishedObjects
                  << ", Received: " << metrics_.receivedObjects << "\n";
        nextProgress += seconds(progressInterval);
      }

      auto elapsed = steady_clock::now() - iterStart;
      if (elapsed < updateInterval) {
        co_await folly::coro::sleep(duration_cast<microseconds>(updateInterval - elapsed));
      }
    }

    if (eventLogger_) {
      eventLogger_->flush();
    }
  }

  void cleanup() {
    // Clear consumers to stop publishing - let everything else be cleaned up
    // by destructors when the test object goes out of scope.
    //
    // NOTE: We intentionally skip explicit session->close() calls here.
    // The MoQSession close path has a lifecycle issue where close() triggers
    // async callbacks that race with our cleanup, causing crashes. This is a
    // known issue that should be tracked separately (see: session close lifecycle).
    // For this test binary, we rely on process exit to clean up connections.
    for (auto& panelist : panelists_) {
      panelist.consumer.reset();
    }
  }

  folly::EventBase* evb_;
  std::shared_ptr<MoQFollyExecutorImpl> moqEvb_;
  TrackFilterMetrics& metrics_;

  // Panelist state: tracks the actual panelist ID alongside connection state.
  // This is critical for correct ranking - the audio level must be computed
  // from the panelist ID, not the vector index, to handle partial connection
  // failures correctly.
  struct PanelistState {
    int panelistId;
    std::unique_ptr<MoQWebTransportClient> client;
    std::shared_ptr<MoQSession> session;
    std::shared_ptr<TrackConsumer> consumer;
    std::shared_ptr<Publisher::SubscribeNamespaceHandle> subscribeHandle;
    std::unique_ptr<SpeechSimulator> speechSim;
    uint64_t lastAudioLevel{0};
  };
  std::vector<PanelistState> panelists_;
  std::unique_ptr<TopNEventLogger> eventLogger_;

  std::vector<std::unique_ptr<MoQWebTransportClient>> subscriberClients_;
  std::vector<std::shared_ptr<MoQSession>> subscriberSessions_;
  std::vector<std::shared_ptr<Publisher::SubscribeNamespaceHandle>> subscriberHandles_;
  std::vector<int> mixedTopNValues_;
};

} // namespace

int main(int argc, char** argv) {
  folly::Init init(&argc, &argv, true);

  if (FLAGS_relay_url.empty()) {
    std::cerr << "Error: --relay_url required\n";
    std::cerr << "Example: ./track_filter_load_test --relay_url=https://localhost:9668/moq-relay "
                 "--insecure\n";
    return 1;
  }

  std::cout << "Track Filter Load Test Configuration:\n";
  std::cout << "  IO Threads: " << FLAGS_num_threads << "\n";
  std::cout << "  ALPN protocols: " << FLAGS_alpns << "\n";
  std::cout << "  Drain period: " << FLAGS_drain_period_ms << "ms\n";
  std::cout << "  Speech mode: " << (FLAGS_speech_mode ? "ON (dynamic ranking)" : "OFF (deterministic)") << "\n";
  if (FLAGS_speech_mode && !FLAGS_topn_event_log.empty()) {
    std::cout << "  Event log: " << FLAGS_topn_event_log << "\n";
  }
  std::cout << "\n";

  TrackFilterMetrics metrics;

  if (FLAGS_num_threads <= 1) {
    // Single-threaded mode (original behavior)
    folly::EventBase evb;
    auto test = std::make_shared<TrackFilterLoadTest>(&evb, metrics);

    co_withExecutor(&evb, test->run()).start().via(&evb).thenTry([test, &evb](const auto&) {
      test->generateReport();
      evb.runAfterDelay([&evb]() { evb.terminateLoopSoon(); }, 500);
    });
    evb.loop();
  } else {
    // Multi-threaded mode: distribute connections across multiple EventBases.
    // This improves scalability for 10K+ connections by avoiding a single
    // EventBase bottleneck for QUIC connection processing.
    auto executor = std::make_unique<folly::IOThreadPoolExecutor>(
        FLAGS_num_threads,
        std::make_shared<folly::NamedThreadFactory>("TrackFilterTest"),
        folly::EventBaseManager::get(),
        folly::IOThreadPoolExecutor::Options().setWaitForAll(true)
    );

    // Use first EventBase as the primary for the test orchestration
    auto evbs = executor->getAllEventBases();
    if (evbs.empty()) {
      std::cerr << "Error: No EventBases created\n";
      return 1;
    }

    auto primaryEvb = evbs[0].get();
    auto test = std::make_shared<TrackFilterLoadTest>(primaryEvb, metrics);

    std::atomic<bool> testComplete{false};
    folly::coro::co_withExecutor(primaryEvb, test->run())
        .start()
        .via(primaryEvb)
        .thenTry([test, &testComplete](const auto&) {
          test->generateReport();
          testComplete = true;
        });

    // Wait for test completion
    while (!testComplete.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Allow cleanup time before destroying executor
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    executor->stop();
  }

  return (metrics.selfReceivedObjects.load() > 0 ||
          metrics.subscriberVerificationFailures.load() > 0 ||
          metrics.panelistVerificationFailures.load() > 0)
             ? 1
             : 0;
}
