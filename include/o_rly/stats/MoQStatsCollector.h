#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include <folly/executors/SequencedExecutor.h>
#include <moxygen/stats/MoQStats.h>

#include <o_rly/stats/BoundedHistogram.h>
#include <o_rly/stats/StatsRegistry.h>

namespace openmoq::o_rly::stats {

class MoQStatsCollector : public moxygen::MoQPublisherStatsCallback,
                          public moxygen::MoQSubscriberStatsCallback,
                          public StatsCollectorBase {
public:
  // Factory: constructs the collector, registers it with the registry, and
  // returns the shared_ptr.
  static std::shared_ptr<MoQStatsCollector> create_moq_stats_collector(
      folly::Executor::KeepAlive<> owningExecutor,
      std::shared_ptr<StatsRegistry> registry
  );

  ~MoQStatsCollector() override;

  StatsSnapshot snapshot() const override;
  folly::Executor::KeepAlive<> owningExecutor() const override;

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

  void recordPublishNamespaceLatency(uint64_t latencyMsec) override;
  void recordPublishLatency(uint64_t latencyMsec) override;
  void onPublishError(moxygen::PublishErrorCode errorCode) override;
  void onPublishSuccess() override;

  void recordSubscribeLatency(uint64_t latencyMsec) override;
  void recordFetchLatency(uint64_t latencyMsec) override;
  void onPublish() override;
  void onPublishOk() override;

  // Session lifecycle events
  void onSessionStart();
  void onSessionEnd();

private:
  // Constructor is private; use create().
  explicit MoQStatsCollector(folly::Executor::KeepAlive<> owningExecutor);

  folly::Executor::KeepAlive<> owningExecutor_;
  std::weak_ptr<StatsRegistry> registry_;

  // Metric Fields
#define DEFINE_FIELD(type, name) type name##_{0};
  STATS_COUNTER_FIELDS(DEFINE_FIELD)
  STATS_GAUGE_FIELDS(DEFINE_FIELD)
#undef DEFINE_FIELD

#define DEFINE_HISTOGRAM(name, bounds) BoundedHistogram<bounds.size()> name##_{bounds};
  STATS_HISTOGRAM_FIELDS(DEFINE_HISTOGRAM)
#undef DEFINE_HISTOGRAM
};

} // namespace openmoq::o_rly::stats
