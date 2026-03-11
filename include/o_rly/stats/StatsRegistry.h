#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <folly/executors/SequencedExecutor.h>
#include <folly/experimental/coro/Task.h>

namespace openmoq::o_rly::stats {

// Latency buckets in microseconds (SLO: < 1000 µs = 1 ms per spec 4.1).
inline constexpr std::array<uint64_t, 11> kLatencyBucketsUs = {
    10, 50, 100, 250, 500, 1000, 2000, 5000, 10000, 50000, 100000};


// uint64_t monotonically-increasing counters
#define STATS_COUNTER_FIELDS(X)             \
  X(uint64_t, moqSubscribeSuccess)          \
  X(uint64_t, moqSubscribeError)            \
  X(uint64_t, moqFetchSuccess)              \
  X(uint64_t, moqFetchError)                \
  X(uint64_t, moqPublishNamespaceSuccess)   \
  X(uint64_t, moqPublishNamespaceError)     \
  X(uint64_t, moqPublishNamespaceDone)      \
  X(uint64_t, moqPublishNamespaceCancel)    \
  X(uint64_t, moqSubscribeNamespaceSuccess) \
  X(uint64_t, moqSubscribeNamespaceError)   \
  X(uint64_t, moqUnsubscribeNamespace)      \
  X(uint64_t, moqPublishSuccess)            \
  X(uint64_t, moqPublishError)              \
  X(uint64_t, moqPublishDone)               \
  X(uint64_t, moqSubscriptionStreamOpened)  \
  X(uint64_t, moqSubscriptionStreamClosed)  \
  X(uint64_t, moqTrackStatus)               \
  X(uint64_t, moqRequestUpdate)             \
  X(uint64_t, moqPublishReceived)           \
  X(uint64_t, moqPublishOkReceived)

// int64_t gauges (can go up and down; negative value signals bookkeeping bug)
#define STATS_GAUGE_FIELDS(X)                \
  X(int64_t, moqActiveSubscriptions)         \
  X(int64_t, moqActivePublishers)            \
  X(int64_t, moqActivePublishNamespaces)     \
  X(int64_t, moqActiveSubscribeNamespaces)   \
  X(int64_t, moqActiveSubscriptionStreams)

// Global gauges (owned by registry, not summed during aggregation)
#define STATS_GLOBAL_GAUGE_FIELDS(X) \
  X(int64_t, moqActiveSessions)

// Histograms: (name, constexpr_bounds_ref)
// Each expands to: name##Buckets[] (len = bounds.size()+1 for +Inf),
//                  name##Sum, name##Count
#define STATS_HISTOGRAM_FIELDS(X)                      \
  X(moqSubscribeLatency, kLatencyBucketsUs)             \
  X(moqFetchLatency,     kLatencyBucketsUs)             \
  X(moqPublishNamespaceLatency, kLatencyBucketsUs)      \
  X(moqPublishLatency,   kLatencyBucketsUs)


struct StatsSnapshot {
  // --- Scalar fields ---
#define DEFINE_FIELD(type, name) type name{0};
  STATS_COUNTER_FIELDS(DEFINE_FIELD)
  STATS_GAUGE_FIELDS(DEFINE_FIELD)
  STATS_GLOBAL_GAUGE_FIELDS(DEFINE_FIELD)
#undef DEFINE_FIELD

  // --- Histogram fields ---
#define DEFINE_HISTOGRAM(name, bounds)                                       \
  std::array<uint64_t, std::tuple_size_v<decltype(bounds)> + 1> name##Buckets{}; \
  uint64_t name##Sum{0};                                                     \
  uint64_t name##Count{0};
  STATS_HISTOGRAM_FIELDS(DEFINE_HISTOGRAM)
#undef DEFINE_HISTOGRAM

  StatsSnapshot& operator+=(const StatsSnapshot& o);

  // --- Prometheus text format ---
  // Returns the full Prometheus exposition text for this snapshot.
  // Emits # HELP / # TYPE / value lines for all fields and histograms.
  static std::string formatPrometheus(const StatsSnapshot& snap);
};


class StatsCollectorBase {
 public:
  virtual ~StatsCollectorBase() = default;

  // Returns a point-in-time copy of all metrics.
  // MUST be called on owningExecutor().
  virtual StatsSnapshot snapshot() const = 0;

  // The executor owned by this collector's producing thread.
  virtual folly::Executor::KeepAlive<> owningExecutor() const = 0;
};

// ---------------------------------------------------------------------------
// StatsRegistry
// Singleton-ish shared owner of all active collectors.  Collectors
// register/deregister themselves; the registry aggregates on demand.
// ---------------------------------------------------------------------------

class StatsRegistry {
 public:
  StatsRegistry() = default;
  ~StatsRegistry() = default;

  // Called by StatsCollectorBase::ctor — registers the collector.
  void registerCollector(std::shared_ptr<StatsCollectorBase> collector);

  // Called by StatsCollectorBase::dtor — deregisters the collector.
  void deregisterCollector(StatsCollectorBase* collector);

  // Global gauge management — called by ORelayServer lifecycle hooks.
  void onNewSession();
  void onTerminateSession();

  // Aggregates all live collectors into a single StatsSnapshot.
  folly::coro::Task<StatsSnapshot> aggregateAsync();

 private:
  mutable std::mutex mu_;
  std::vector<std::shared_ptr<StatsCollectorBase>> collectors_;

  // --- Global gauges ---
#define DEFINE_GLOBAL_GAUGE(type, name) type name##_{0};
  STATS_GLOBAL_GAUGE_FIELDS(DEFINE_GLOBAL_GAUGE)
#undef DEFINE_GLOBAL_GAUGE
};

} // namespace openmoq::o_rly::stats
