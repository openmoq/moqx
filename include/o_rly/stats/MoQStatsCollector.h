#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include <folly/executors/SequencedExecutor.h>
#include <moxygen/stats/MoQStats.h>

#include <o_rly/stats/StatsRegistry.h>

namespace openmoq::o_rly::stats {

// ---------------------------------------------------------------------------
// BoundedHistogram
//
// Lightweight fixed-boundary histogram.  Stores per-range (non-cumulative)
// counts internally; snapshot() converts to Prometheus cumulative le-buckets.
//
// Template parameter N is the number of explicit boundaries.  Storage is:
//   buckets[0..N-1] = count of observations in each range
//   buckets[N]      = count of observations above the last boundary (+Inf)
// ---------------------------------------------------------------------------

template <size_t N>
struct BoundedHistogram {
  explicit BoundedHistogram(const std::array<uint64_t, N>& b) : bounds(b) {}

  void addValue(uint64_t val) {
    ++count;
    sum += val;
    for (size_t i = 0; i < N; ++i) {
      if (val <= bounds[i]) {
        ++buckets[i];
        return;
      }
    }
    ++buckets[N]; // above all explicit bounds → +Inf bucket
  }

  // Fill a Prometheus-style cumulative bucket array of size N+1.
  // cumulative[i] = # observations <= bounds[i]
  template <size_t M>
  void fillCumulative(std::array<uint64_t, M>& out) const {
    static_assert(M == N + 1, "output array must be N+1");
    uint64_t running = 0;
    for (size_t i = 0; i < N; ++i) {
      running += buckets[i];
      out[i] = running;
    }
    out[N] = count; // every observation is <= +Inf
  }

  const std::array<uint64_t, N>& bounds;
  std::array<uint64_t, N + 1> buckets{};
  uint64_t sum{0};
  uint64_t count{0};
};

// ---------------------------------------------------------------------------
// MoQStatsCollector
//
// One instance per MoQSession.  Created in ORelayServer::onNewSession()
//
// Both callbacks share the same collector so the relay accounts for each
// session once regardless of which role fires the callback.
//
// The collector registers itself with StatsRegistry on construction and
// deregisters on destruction.
// ---------------------------------------------------------------------------

class MoQStatsCollector
    : public moxygen::MoQPublisherStatsCallback,
      public moxygen::MoQSubscriberStatsCallback,
      public StatsCollectorBase {
 public:
  // owningExecutor: the relay's folly EventBase (or equivalent executor).
  // registry: shared registry; the collector registers itself here.
  MoQStatsCollector(
      folly::Executor::KeepAlive<> owningExecutor,
      std::shared_ptr<StatsRegistry> registry);

  ~MoQStatsCollector() override;

  StatsSnapshot snapshot() const override;
  folly::Executor::KeepAlive<> owningExecutor() const override;

  void onSubscribeSuccess() override;
  void onSubscribeError(moxygen::SubscribeErrorCode errorCode) override;
  void onFetchSuccess() override;
  void onFetchError(moxygen::FetchErrorCode errorCode) override;
  void onPublishNamespaceSuccess() override;
  void onPublishNamespaceError(
      moxygen::PublishNamespaceErrorCode errorCode) override;
  void onPublishNamespaceDone() override;
  void onPublishNamespaceCancel() override;
  void onSubscribeNamespaceSuccess() override;
  void onSubscribeNamespaceError(
      moxygen::SubscribeNamespaceErrorCode errorCode) override;
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

 private:
  folly::Executor::KeepAlive<> owningExecutor_;
  std::weak_ptr<StatsRegistry> registry_;

// Metric Fields
#define DEFINE_FIELD(type, name) type name##_{0};
  STATS_COUNTER_FIELDS(DEFINE_FIELD)
  STATS_GAUGE_FIELDS(DEFINE_FIELD)
#undef DEFINE_FIELD

#define DEFINE_HISTOGRAM(name, bounds) \
  BoundedHistogram<bounds.size()> name##_{bounds};
  STATS_HISTOGRAM_FIELDS(DEFINE_HISTOGRAM)
#undef DEFINE_HISTOGRAM
};

} // namespace openmoq::o_rly::stats
