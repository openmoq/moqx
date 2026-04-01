#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

#include <folly/executors/SequencedExecutor.h>
#include <moxygen/stats/MoQStats.h>

#include <moqx/stats/BoundedHistogram.h>
#include <moqx/stats/StatsRegistry.h>

namespace openmoq::moqx::stats {

// MoQStatsCollector owns all metric storage and acts as the StatsCollectorBase
// registered with the StatsRegistry.  It exposes two inner callback objects:
//
//   PublisherCallback  – implements MoQPublisherStatsCallback
//                        Relay uses this when it is the *publisher*
//                        (serving downstream subscribers).
//
//   SubscriberCallback – implements MoQSubscriberStatsCallback
//                        Relay uses this when it is the *subscriber*
//                        (consuming from upstream publishers).
class MoQStatsCollector : public StatsCollectorBase {
public:
  // --- Inner callback: publisher role ---
  class PublisherCallback : public moxygen::MoQPublisherStatsCallback {
  public:
    explicit PublisherCallback(MoQStatsCollector& parent) : parent_(parent) {}

    // Shared-base methods – publisher semantics
    void onSubscribeSuccess() override;
    void onSubscribeError(moxygen::SubscribeErrorCode errorCode) override;
    void onFetchSuccess() override;
    void onFetchError(moxygen::FetchErrorCode errorCode) override;
    void onPublishNamespaceSuccess() override;
    void onPublishNamespaceError(moxygen::PublishNamespaceErrorCode errorCode) override;
    void onPublishNamespaceDone() override;
    void onPublishNamespaceCancel() override;
    void onSubscribeNamespaceSuccess() override;
    void onSubscribeNamespaceError(moxygen::SubscribeNamespaceErrorCode errorCode) override;
    void onUnsubscribeNamespace() override;
    void onTrackStatus() override;
    void onUnsubscribe() override;
    void onPublishDone(moxygen::PublishDoneStatusCode statusCode) override;
    void onRequestUpdate() override;
    void onSubscriptionStreamOpened() override;
    void onSubscriptionStreamClosed() override;

    // Publisher-only methods
    void recordPublishNamespaceLatency(uint64_t latencyMsec) override;
    void recordPublishLatency(uint64_t latencyMsec) override;
    void onPublishError(moxygen::PublishErrorCode errorCode) override;
    void onPublishSuccess() override;

  private:
    MoQStatsCollector& parent_;
  };

  // --- Inner callback: subscriber role ---
  class SubscriberCallback : public moxygen::MoQSubscriberStatsCallback {
  public:
    explicit SubscriberCallback(MoQStatsCollector& parent) : parent_(parent) {}

    // Shared-base methods – subscriber semantics
    void onSubscribeSuccess() override;
    void onSubscribeError(moxygen::SubscribeErrorCode errorCode) override;
    void onFetchSuccess() override;
    void onFetchError(moxygen::FetchErrorCode errorCode) override;
    void onPublishNamespaceSuccess() override;
    void onPublishNamespaceError(moxygen::PublishNamespaceErrorCode errorCode) override;
    void onPublishNamespaceDone() override;
    void onPublishNamespaceCancel() override;
    void onSubscribeNamespaceSuccess() override;
    void onSubscribeNamespaceError(moxygen::SubscribeNamespaceErrorCode errorCode) override;
    void onUnsubscribeNamespace() override;
    void onTrackStatus() override;
    void onUnsubscribe() override;
    void onPublishDone(moxygen::PublishDoneStatusCode statusCode) override;
    void onRequestUpdate() override;
    void onSubscriptionStreamOpened() override;
    void onSubscriptionStreamClosed() override;

    // Subscriber-only methods
    void recordSubscribeLatency(uint64_t latencyMsec) override;
    void recordFetchLatency(uint64_t latencyMsec) override;
    void onPublish() override;
    void onPublishOk() override;
    void onPublishError(moxygen::PublishErrorCode errorCode) override;

  private:
    MoQStatsCollector& parent_;
  };

  // --- Factory ---
  static std::shared_ptr<MoQStatsCollector>
  create_moq_stats_collector(std::shared_ptr<StatsRegistry> registry);

  ~MoQStatsCollector() override = default;

  void setExecutor(folly::Executor* executor);

  // Returns aliased shared_ptrs whose reference count is shared with this
  // MoQStatsCollector.  Holding only an inner callback keeps the parent alive.
  std::shared_ptr<moxygen::MoQPublisherStatsCallback> publisherCallback() const;
  std::shared_ptr<moxygen::MoQSubscriberStatsCallback> subscriberCallback() const;

  StatsSnapshot snapshot() const override;
  folly::Executor* owningExecutor() const override;

  // Session lifecycle events (called directly by MoqxRelayServer).
  void onSessionStart();
  void onSessionEnd();

private:
  MoQStatsCollector();

  // Atomic so the admin evb can read it safely while onNewSession sets it
  // from the MoQ session thread.
  std::atomic<folly::Executor*> owningExecutor_{nullptr};

  // Value-member inner callbacks.
  PublisherCallback pubCallback_;
  SubscriberCallback subCallback_;

  // Aliased shared_ptrs set up in create_moq_stats_collector().
  std::shared_ptr<moxygen::MoQPublisherStatsCallback> pubCallbackPtr_;
  std::shared_ptr<moxygen::MoQSubscriberStatsCallback> subCallbackPtr_;

  // --- Metric storage ---
#define DEFINE_FIELD(type, name) type name##_{0};
  STATS_MOQ_COUNTER_FIELDS(DEFINE_FIELD)
  STATS_GAUGE_FIELDS(DEFINE_FIELD)
#undef DEFINE_FIELD

#define DEFINE_HISTOGRAM(name, bounds) BoundedHistogram<bounds.size()> name##_{bounds};
  STATS_HISTOGRAM_FIELDS(DEFINE_HISTOGRAM)
#undef DEFINE_HISTOGRAM

  // Per-RequestErrorCode breakdown arrays (parallel to the aggregate counters).
#define DEFINE_ERROR_ARRAY(name) std::array<uint64_t, kRequestErrorCodeCount> name##ByCodes_{};
  STATS_ERROR_COUNTER_FIELDS(DEFINE_ERROR_ARRAY)
#undef DEFINE_ERROR_ARRAY
};

} // namespace openmoq::moqx::stats
