#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <folly/executors/SequencedExecutor.h>
#include <folly/experimental/coro/Task.h>
#include <folly/io/IOBuf.h>

#include <moxygen/MoQTypes.h>

namespace openmoq::moqx::stats {

// Latency buckets in microseconds (SLO: < 1000 µs = 1 ms per spec 4.1).
inline constexpr std::array<uint64_t, 11> kLatencyBucketsUs =
    {10, 50, 100, 250, 500, 1000, 2000, 5000, 10000, 50000, 100000};

// RequestErrorCode compact-index table
// All *ErrorCode type aliases resolve to moxygen::RequestErrorCode. we maintain a
// small ordered lookup table and a constexpr mapping function.  Unknown codes
// fall into the last slot (CANCELLED).
inline constexpr std::array<moxygen::RequestErrorCode, 8> kRequestErrorCodeValues = {{
    moxygen::RequestErrorCode::INTERNAL_ERROR,
    moxygen::RequestErrorCode::UNAUTHORIZED,
    moxygen::RequestErrorCode::TIMEOUT,
    moxygen::RequestErrorCode::NOT_SUPPORTED,
    moxygen::RequestErrorCode::TRACK_NOT_EXIST,
    moxygen::RequestErrorCode::INVALID_RANGE,
    moxygen::RequestErrorCode::GOING_AWAY,
    moxygen::RequestErrorCode::CANCELLED,
}};

inline constexpr size_t kRequestErrorCodeCount = kRequestErrorCodeValues.size();

// Maps any RequestErrorCode to a compact index in [0, kRequestErrorCodeCount).
// Unknown / future codes fall into the last slot (CANCELLED).
constexpr size_t requestErrorCodeIndex(moxygen::RequestErrorCode code) {
  for (size_t i = 0; i < kRequestErrorCodeCount - 1; ++i) {
    if (kRequestErrorCodeValues[i] == code) {
      return i;
    }
  }
  return kRequestErrorCodeCount - 1;
}

// Prometheus label values for each RequestErrorCode slot.
inline constexpr std::array<std::string_view, 8> kRequestErrorCodeLabels = {{
    "internal_error",
    "unauthorized",
    "timeout",
    "not_supported",
    "track_not_exist",
    "invalid_range",
    "going_away",
    "cancelled",
}};

// uint64_t monotonically-increasing counters.
// pub* = relay acting as publisher (serving downstream subscribers).
// sub* = relay acting as subscriber (consuming from upstream publishers).
// moq* = unambiguously tied to one role (no pub/sub prefix needed).
#define STATS_COUNTER_FIELDS(X)                                                                    \
  /* Publisher-side: relay accepted/rejected subscription requests */                              \
  X(uint64_t, pubSubscribeSuccess)                                                                 \
  X(uint64_t, pubSubscribeError)                                                                   \
  X(uint64_t, pubFetchSuccess)                                                                     \
  X(uint64_t, pubFetchError)                                                                       \
  X(uint64_t, pubPublishNamespaceSuccess)                                                          \
  X(uint64_t, pubPublishNamespaceError)                                                            \
  X(uint64_t, pubPublishNamespaceDone)                                                             \
  X(uint64_t, pubPublishNamespaceCancel)                                                           \
  X(uint64_t, pubSubscribeNamespaceSuccess)                                                        \
  X(uint64_t, pubSubscribeNamespaceError)                                                          \
  X(uint64_t, pubUnsubscribeNamespace)                                                             \
  X(uint64_t, pubPublishDone)                                                                      \
  X(uint64_t, pubSubscriptionStreamOpened)                                                         \
  X(uint64_t, pubSubscriptionStreamClosed)                                                         \
  X(uint64_t, pubTrackStatus)                                                                      \
  X(uint64_t, pubRequestUpdate)                                                                    \
  /* Publisher-only methods: relay sent PUBLISH, received PUBLISH_OK/ERROR back */                 \
  X(uint64_t, moqPublishSuccess)                                                                   \
  X(uint64_t, moqPublishError)                                                                     \
  /* Subscriber-side: relay subscribed to / was rejected by upstream */                            \
  X(uint64_t, subSubscribeSuccess)                                                                 \
  X(uint64_t, subSubscribeError)                                                                   \
  X(uint64_t, subFetchSuccess)                                                                     \
  X(uint64_t, subFetchError)                                                                       \
  X(uint64_t, subPublishNamespaceSuccess)                                                          \
  X(uint64_t, subPublishNamespaceError)                                                            \
  X(uint64_t, subPublishNamespaceDone)                                                             \
  X(uint64_t, subPublishNamespaceCancel)                                                           \
  X(uint64_t, subSubscribeNamespaceSuccess)                                                        \
  X(uint64_t, subSubscribeNamespaceError)                                                          \
  X(uint64_t, subUnsubscribeNamespace)                                                             \
  X(uint64_t, subPublishDone)                                                                      \
  X(uint64_t, subSubscriptionStreamOpened)                                                         \
  X(uint64_t, subSubscriptionStreamClosed)                                                         \
  X(uint64_t, subTrackStatus)                                                                      \
  X(uint64_t, subRequestUpdate)                                                                    \
  /* Subscriber-only methods: upstream publisher connected to relay */                             \
  X(uint64_t, moqPublishReceived)                                                                  \
  X(uint64_t, moqPublishOkSent)                                                                    \
  X(uint64_t, subPublishError)

// int64_t gauges (can go up and down; negative value signals bookkeeping bug).
// pub*/sub* split mirrors the counter naming above.
#define STATS_GAUGE_FIELDS(X)                                                                      \
  X(int64_t, pubActiveSubscriptions)                                                               \
  X(int64_t, pubActivePublishers)                                                                  \
  X(int64_t, pubActivePublishNamespaces)                                                           \
  X(int64_t, pubActiveSubscribeNamespaces)                                                         \
  X(int64_t, pubActiveSubscriptionStreams)                                                         \
  X(int64_t, subActiveSubscriptions)                                                               \
  X(int64_t, subActivePublishers)                                                                  \
  X(int64_t, subActivePublishNamespaces)                                                           \
  X(int64_t, subActiveSubscribeNamespaces)                                                         \
  X(int64_t, subActiveSubscriptionStreams)                                                         \
  X(int64_t, moqActiveSessions)

// Histograms: (name, constexpr_bounds_ref)
// Each expands to: name##Buckets[] (len = bounds.size()+1 for +Inf),
//                  name##Sum, name##Count
#define STATS_HISTOGRAM_FIELDS(X)                                                                  \
  X(moqSubscribeLatency, kLatencyBucketsUs)                                                        \
  X(moqFetchLatency, kLatencyBucketsUs)                                                            \
  X(moqPublishNamespaceLatency, kLatencyBucketsUs)                                                 \
  X(moqPublishLatency, kLatencyBucketsUs)

// Error-code breakdowns: fields in STATS_COUNTER_FIELDS whose callbacks receive
// a RequestErrorCode argument.  Each expands to a
// std::array<uint64_t, kRequestErrorCodeCount> named  name##ByCodes.
#define STATS_ERROR_COUNTER_FIELDS(X)                                                              \
  X(pubSubscribeError)                                                                             \
  X(pubFetchError)                                                                                 \
  X(pubPublishNamespaceError)                                                                      \
  X(pubSubscribeNamespaceError)                                                                    \
  X(moqPublishError)                                                                               \
  X(subSubscribeError)                                                                             \
  X(subFetchError)                                                                                 \
  X(subPublishNamespaceError)                                                                      \
  X(subSubscribeNamespaceError)                                                                    \
  X(subPublishError)

struct StatsSnapshot {
  // --- Scalar fields ---
#define DEFINE_FIELD(type, name) type name{0};
  STATS_COUNTER_FIELDS(DEFINE_FIELD)
  STATS_GAUGE_FIELDS(DEFINE_FIELD)
#undef DEFINE_FIELD

  // --- Histogram fields ---
#define DEFINE_HISTOGRAM(name, bounds)                                                             \
  std::array<uint64_t, std::tuple_size_v<decltype(bounds)> + 1> name##Buckets{};                   \
  uint64_t name##Sum{0};                                                                           \
  uint64_t name##Count{0};
  STATS_HISTOGRAM_FIELDS(DEFINE_HISTOGRAM)
#undef DEFINE_HISTOGRAM

  // --- Per-RequestErrorCode breakdown arrays ---
#define DEFINE_ERROR_ARRAY(name) std::array<uint64_t, kRequestErrorCodeCount> name##ByCodes{};
  STATS_ERROR_COUNTER_FIELDS(DEFINE_ERROR_ARRAY)
#undef DEFINE_ERROR_ARRAY

  StatsSnapshot& operator+=(const StatsSnapshot& o);

  // --- Prometheus text format ---
  static std::unique_ptr<folly::IOBuf> formatPrometheus(const StatsSnapshot& snap);
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

  // Register a collector. Must be called before lock().
  void registerCollector(std::shared_ptr<StatsCollectorBase> collector);

  // Seal the registry. Any further registerCollector calls will XLOG(FATAL).
  // Call this explicitly once all collectors have been registered.
  void lock();

  // Called by StatsCollectorBase::dtor — deregisters the collector.
  void deregisterCollector(StatsCollectorBase* collector);

  folly::coro::Task<StatsSnapshot> aggregateAsync();

private:
  bool locked_ = false;
  std::vector<std::shared_ptr<StatsCollectorBase>> collectors_;
};

} // namespace openmoq::moqx::stats
