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

#include <folly/coro/BlockingWait.h>
#include <folly/coro/Collect.h>
#include <folly/coro/Sleep.h>
#include <folly/init/Init.h>
#include <folly/portability/GFlags.h>
#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <folly/synchronization/Synchronized.h>
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
#include <sstream>
#include <vector>

using namespace moxygen;
using namespace std::chrono;

DEFINE_string(relay_url, "", "Relay URL");
DEFINE_int32(panelists, 100, "Number of panelists (each is publisher + subscriber)");
DEFINE_int32(subscribers, 10000, "Number of pure subscribers");
DEFINE_int32(top_n, 3, "Top-N value for TRACK_FILTER");
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

namespace {

constexpr uint64_t kAudioLevelPropertyType = 0x01;

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

  struct LatencyState {
    std::vector<uint64_t> objectLatencies;
    std::vector<uint64_t> selectionLatencies;
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

  // Latency tracking (microseconds)
  folly::Synchronized<LatencyState> latencyState;

  // Timing
  steady_clock::time_point testStart;
  steady_clock::time_point testEnd;

  void recordObjectLatency(uint64_t latencyUs) {
    auto latency = latencyState.wlock();
    if (latency->objectLatencies.size() < 1000000) { // Cap at 1M samples
      latency->objectLatencies.push_back(latencyUs);
    }
  }

  void recordSelectionLatency(uint64_t latencyUs) {
    auto latency = latencyState.wlock();
    if (latency->selectionLatencies.size() < 100000) {
      latency->selectionLatencies.push_back(latencyUs);
    }
  }

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
  folly::coro::Task<RequestUpdateResult> requestUpdate(RequestUpdate) override {
    co_return folly::makeUnexpected(
        RequestError{RequestID(0), RequestErrorCode::INTERNAL_ERROR, "N/A"}
    );
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
      : evb_(evb), moqEvb_(std::make_shared<MoQFollyExecutorImpl>(evb)), metrics_(metrics) {}

  folly::coro::Task<void> run() {
    metrics_.testStart = steady_clock::now();

    std::cout << "Track Filter Load Test\n";
    std::cout << "======================\n";
    std::cout << "Panelists: " << FLAGS_panelists << " (pub+sub with self-exclusion)\n";
    std::cout << "Subscribers: " << FLAGS_subscribers << " (pure subscribers)\n";
    std::cout << "Top-N: " << FLAGS_top_n << "\n";
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

      if (panelistConsumers_.empty()) {
        std::cerr << "Error: No panelists connected\n";
        co_return;
      }

      // Phase 3: Run publishing loop
      std::cout << "\nPhase 3: Running test for " << FLAGS_duration << " seconds...\n";
      co_await runPublisherLoop(seconds(FLAGS_duration));
      verifyCorrectness();

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
    report << "  Top-N Filter:            " << FLAGS_top_n << "\n";
    report << "  Ranking Mode:            deterministic descending by panelist id\n";
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

    report << "TOP-N CORRECTNESS VERIFICATION\n";
    report << "------------------------------\n";
    report << "  Subscribers Verified:     " << metrics_.subscribersVerified << "\n";
    report << "  Subscriber Failures:      " << metrics_.subscriberVerificationFailures << "\n";
    report << "  Panelists Verified:       " << metrics_.panelistsVerified << "\n";
    report << "  Panelist Failures:        " << metrics_.panelistVerificationFailures << "\n";
    report << "  Overall Status:           "
           << ((metrics_.subscriberVerificationFailures == 0 &&
                metrics_.panelistVerificationFailures == 0 &&
                metrics_.selfReceivedObjects == 0)
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

    // Latency Statistics
    {
      auto latency = metrics_.latencyState.wlock();
      if (!latency->objectLatencies.empty()) {
        report << "OBJECT LATENCY (microseconds)\n";
        report << "-----------------------------\n";
        std::sort(latency->objectLatencies.begin(), latency->objectLatencies.end());
        size_t n = latency->objectLatencies.size();
        report << "  Samples:    " << n << "\n";
        report << "  Min:        " << latency->objectLatencies.front() << " us\n";
        report << "  Max:        " << latency->objectLatencies.back() << " us\n";
        report << "  P50:        " << latency->objectLatencies[n / 2] << " us\n";
        report << "  P95:        " << latency->objectLatencies[n * 95 / 100] << " us\n";
        report << "  P99:        " << latency->objectLatencies[n * 99 / 100] << " us\n\n";
      }
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
    std::vector<int> panelists(state->connectedPanelistIds.begin(), state->connectedPanelistIds.end());
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
      size_t maxSamples) {
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
        state->connectedPanelistIds.begin(), state->connectedPanelistIds.end());
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
        state->panelistFailures.push_back(
            TrackFilterMetrics::VerificationFailure{
                .clientId = id, .expected = std::move(expected), .actual = std::move(actual)}
        );
      }
    }

    std::vector<int> connectedSubscribers(
        state->connectedSubscriberIds.begin(), state->connectedSubscriberIds.end());
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
        state->subscriberFailures.push_back(
            TrackFilterMetrics::VerificationFailure{
                .clientId = id, .expected = expectedSubs, .actual = std::move(actual)}
        );
      }
    }
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
      std::vector<std::string> alpns = {"moqt-16", "moqt-15", "moq-00"};
      co_await client->setupMoQSession(
          milliseconds(FLAGS_connect_timeout),
          seconds(FLAGS_transaction_timeout),
          nullptr,
          handler,
          quic::TransportSettings(),
          alpns
      );

      auto session = client->moqSession_;
      if (!session) {
        co_return false;
      }

      // Increase max request ID to support many concurrent subscriptions
      session->setMaxConcurrentRequests(10000);

      // Subscribe with TRACK_FILTER
      SubscribeNamespace subNs;
      subNs.requestID = RequestID{static_cast<uint64_t>(id * 2)};
      subNs.trackNamespacePrefix =
          TrackNamespace(std::vector<std::string>{FLAGS_namespace_prefix, "audio"});
      subNs.forward = true;

      TrackFilter filter(kAudioLevelPropertyType, FLAGS_top_n);
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

      panelistClients_.push_back(std::move(client));
      panelistSessions_.push_back(session);
      panelistConsumers_.push_back(pubResponse.consumer);
      subscribeNamespaceHandles_.push_back(std::move(subResult.value()));
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
      std::vector<std::string> alpns = {"moqt-16", "moqt-15", "moq-00"};
      co_await client->setupMoQSession(
          milliseconds(FLAGS_connect_timeout),
          seconds(FLAGS_transaction_timeout),
          nullptr,
          handler,
          quic::TransportSettings(),
          alpns
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

      TrackFilter filter(kAudioLevelPropertyType, FLAGS_top_n);
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

    while (steady_clock::now() < endTime) {
      auto iterStart = steady_clock::now();

      for (size_t i = 0; i < panelistConsumers_.size(); ++i) {
        auto& consumer = panelistConsumers_[i];
        if (!consumer)
          continue;

        // Deterministic strict ranking:
        // panelist-0 > panelist-1 > panelist-2 > ...
        uint64_t audioLevel = static_cast<uint64_t>(panelistConsumers_.size() - i);

        Extensions extensions;
        extensions.getMutableExtensions().push_back(Extension(kAudioLevelPropertyType, audioLevel));

        ObjectHeader
            header{group, 0, 0, 0, ObjectStatus::NORMAL, std::move(extensions), std::nullopt};

        auto payload = folly::IOBuf::copyBuffer(folly::to<std::string>("audio:", audioLevel));

        auto res = consumer->objectStream(header, std::move(payload));
        if (res.hasError()) {
          metrics_.forwardErrors++;
        } else {
          metrics_.publishedObjects++;
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
  }

  void cleanup() {
    // Just clear consumers to stop publishing - let everything else
    // be cleaned up by destructors when the test object goes out of scope.
    // Explicit session->close() triggers async callbacks that race with
    // our cleanup, causing crashes.
    panelistConsumers_.clear();
  }

  folly::EventBase* evb_;
  std::shared_ptr<MoQFollyExecutorImpl> moqEvb_;
  TrackFilterMetrics& metrics_;

  std::vector<std::unique_ptr<MoQWebTransportClient>> panelistClients_;
  std::vector<std::shared_ptr<MoQSession>> panelistSessions_;
  std::vector<std::shared_ptr<TrackConsumer>> panelistConsumers_;
  std::vector<std::shared_ptr<Publisher::SubscribeNamespaceHandle>> subscribeNamespaceHandles_;

  std::vector<std::unique_ptr<MoQWebTransportClient>> subscriberClients_;
  std::vector<std::shared_ptr<MoQSession>> subscriberSessions_;
  std::vector<std::shared_ptr<Publisher::SubscribeNamespaceHandle>> subscriberHandles_;
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

  TrackFilterMetrics metrics;
  folly::EventBase evb;

  auto test = std::make_shared<TrackFilterLoadTest>(&evb, metrics);

  co_withExecutor(&evb, test->run()).start().via(&evb).thenTry([test, &evb](const auto&) {
    test->generateReport();
    // Schedule termination after a short delay to allow graceful cleanup
    evb.runAfterDelay([&evb]() { evb.terminateLoopSoon(); }, 500); // 500ms for cleanup
  });
  evb.loop();

  return (metrics.selfReceivedObjects.load() > 0 ||
          metrics.subscriberVerificationFailures.load() > 0 ||
          metrics.panelistVerificationFailures.load() > 0)
      ? 1
      : 0;
}
