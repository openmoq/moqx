/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <folly/coro/Task.h>
#include <folly/executors/SequencedExecutor.h>
#include <folly/io/IOBuf.h>
#include <folly/io/async/EventBaseLocal.h>

#include <moxygen/MoQTypes.h>

namespace openmoq::moqx::stats {

// Latency buckets in microseconds (SLO: < 1000 µs = 1 ms per spec 4.1).
inline constexpr std::array<uint64_t, 11> kLatencyBucketsUs =
    {10, 50, 100, 250, 500, 1000, 2000, 5000, 10000, 50000, 100000};

// RTT buckets in milliseconds (from onRttSample).
inline constexpr std::array<uint64_t, 10> kRttBucketsMs =
    {1, 5, 10, 25, 50, 100, 250, 500, 1000, 5000};

// Bandwidth buckets in bits/second (from onBandwidthSample).
inline constexpr std::array<uint64_t, 10> kBandwidthBucketsBitsPerSec = {
    1000000,
    5000000,
    10000000,
    25000000,
    50000000,
    100000000,
    250000000,
    500000000,
    1000000000,
    10000000000ULL
};

// Bytes-in-flight buckets in bytes (from onInflightBytesSample).
inline constexpr std::array<uint64_t, 7> kInflightBytesBuckets =
    {4096, 65536, 262144, 1048576, 4194304, 16777216, 67108864};

// EventBase loop time buckets in microseconds.
inline constexpr std::array<uint64_t, 11> kEvbLoopBucketsUs =
    {50, 100, 250, 500, 1000, 2500, 5000, 10000, 25000, 50000, 100000};

// Per-loop QUIC packet count buckets.
inline constexpr std::array<uint64_t, 12> kEvbPktsPerLoopBuckets =
    {0, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024};

// RequestErrorCode compact-index table
// All *ErrorCode type aliases resolve to moxygen::RequestErrorCode. we maintain a
// small ordered lookup table and a constexpr mapping function.  Unknown codes
// fall into the last slot (CANCELLED).
inline constexpr std::array<moxygen::RequestErrorCode, 8> kRequestErrorCodeValues = {{
    moxygen::RequestErrorCode::INTERNAL_ERROR,
    moxygen::RequestErrorCode::UNAUTHORIZED,
    moxygen::RequestErrorCode::TIMEOUT,
    moxygen::RequestErrorCode::NOT_SUPPORTED,
    moxygen::RequestErrorCode::DOES_NOT_EXIST,
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

// ResetStreamErrorCode compact-index table, for per-reason breakdown of
// subgroup/stream resets.  Mirrors the RequestErrorCode table above, but adds a
// trailing "unknown" slot so unrecognized/future codes don't collide with a
// real reason.
inline constexpr std::array<moxygen::ResetStreamErrorCode, 10> kResetStreamErrorCodeValues = {{
    moxygen::ResetStreamErrorCode::INTERNAL_ERROR,
    moxygen::ResetStreamErrorCode::CANCELLED,
    moxygen::ResetStreamErrorCode::DELIVERY_TIMEOUT,
    moxygen::ResetStreamErrorCode::SESSION_CLOSED,
    moxygen::ResetStreamErrorCode::GOING_AWAY,
    moxygen::ResetStreamErrorCode::TOO_FAR_BEHIND,
    moxygen::ResetStreamErrorCode::UNKNOWN_OBJECT_STATUS,
    moxygen::ResetStreamErrorCode::EXPIRED_AUTH_TOKEN,
    moxygen::ResetStreamErrorCode::EXCESSIVE_LOAD,
    moxygen::ResetStreamErrorCode::MALFORMED_TRACK,
}};

inline constexpr size_t kResetStreamErrorCodeCount = kResetStreamErrorCodeValues.size() + 1;

// Maps any ResetStreamErrorCode to a compact index in
// [0, kResetStreamErrorCodeCount).  Unknown / future codes fall into the last
// ("unknown") slot.
constexpr size_t resetStreamErrorCodeIndex(moxygen::ResetStreamErrorCode code) {
  for (size_t i = 0; i < kResetStreamErrorCodeValues.size(); ++i) {
    if (kResetStreamErrorCodeValues[i] == code) {
      return i;
    }
  }
  return kResetStreamErrorCodeCount - 1;
}

// Prometheus label values for each ResetStreamErrorCode slot.
inline constexpr std::array<std::string_view, kResetStreamErrorCodeCount>
    kResetStreamErrorCodeLabels = {{
        "internal_error",
        "cancelled",
        "delivery_timeout",
        "session_closed",
        "going_away",
        "too_far_behind",
        "unknown_object_status",
        "expired_auth_token",
        "excessive_load",
        "malformed_track",
        "unknown",
    }};

// uint64_t monotonically-increasing counters — MoQ application layer.
// pub* = relay acting as publisher (serving downstream subscribers).
// sub* = relay acting as subscriber (consuming from upstream publishers).
// moq* = unambiguously tied to one role (no pub/sub prefix needed).
#define STATS_MOQ_COUNTER_FIELDS(X)                                                                \
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
  X(uint64_t, pubUnsubscribe)                                                                      \
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
  X(uint64_t, subUnsubscribe)                                                                      \
  X(uint64_t, subPublishDone)                                                                      \
  X(uint64_t, subSubscriptionStreamOpened)                                                         \
  X(uint64_t, subSubscriptionStreamClosed)                                                         \
  X(uint64_t, subTrackStatus)                                                                      \
  X(uint64_t, subRequestUpdate)                                                                    \
  /* Subscriber-only methods: upstream publisher connected to relay */                             \
  X(uint64_t, moqPublishReceived)                                                                  \
  X(uint64_t, moqPublishOkSent)                                                                    \
  X(uint64_t, subPublishError)

// uint64_t monotonically-increasing counters — QUIC transport layer.
// Populated exclusively by QuicStatsCollector
#define STATS_QUIC_COUNTER_FIELDS(X)                                                               \
  X(uint64_t, quicPacketsReceived)                                                                 \
  X(uint64_t, quicPacketsSent)                                                                     \
  X(uint64_t, quicPacketsDropped)                                                                  \
  X(uint64_t, quicPacketLoss)                                                                      \
  X(uint64_t, quicPacketRetransmissions)                                                           \
  X(uint64_t, quicConnectionsCreated)                                                              \
  X(uint64_t, quicConnectionsClosed)                                                               \
  X(uint64_t, quicStreamsCreated)                                                                  \
  X(uint64_t, quicStreamsClosed)                                                                   \
  X(uint64_t, quicStreamsReset)                                                                    \
  X(uint64_t, quicConnFlowControlBlocked)                                                          \
  X(uint64_t, quicStreamFlowControlBlocked)                                                        \
  X(uint64_t, quicCwndBlocked)                                                                     \
  X(uint64_t, quicBytesRead)                                                                       \
  X(uint64_t, quicBytesWritten)                                                                    \
  X(uint64_t, quicDatagramsDroppedOnWrite)                                                         \
  X(uint64_t, quicDatagramsDroppedOnRead)                                                          \
  X(uint64_t, quicPeerMaxUniStreamsLimitSaturated)                                                 \
  X(uint64_t, quicPeerMaxBidiStreamsLimitSaturated)                                                \
  X(uint64_t, quicSocketWriteAgain)                                                                \
  X(uint64_t, quicSocketWriteNobufs)                                                               \
  X(uint64_t, quicSocketWriteOther)                                                                \
  X(uint64_t, quicPacketsProcessed)                                                                \
  X(uint64_t, quicPTO)                                                                             \
  X(uint64_t, quicPacketSpuriousLoss)                                                              \
  X(uint64_t, quicPersistentCongestion)                                                            \
  X(uint64_t, quicConnectionWritableBytesLimited)                                                  \
  X(uint64_t, quicConnectionRateLimited)                                                           \
  X(uint64_t, quicPacerTimerLagged)

// Combined convenience macro to iterate all counter fields
#define STATS_COUNTER_FIELDS(X) STATS_MOQ_COUNTER_FIELDS(X) STATS_QUIC_COUNTER_FIELDS(X)

// int64_t gauges — QUIC transport layer
#define STATS_QUIC_GAUGE_FIELDS(X)                                                                 \
  X(int64_t, quicActiveConnections)                                                                \
  X(int64_t, quicActiveStreams)

// int64_t gauges — MoQ application layer
#define STATS_MOQ_GAUGE_FIELDS(X)                                                                  \
  X(int64_t, pubActiveSubscriptions)                                                               \
  X(int64_t, pubActiveSubscriptionStreams)                                                         \
  X(int64_t, subActivePublishers)                                                                  \
  X(int64_t, subActiveSubscriptionStreams)                                                         \
  X(int64_t, moqActiveSessions)

// Combined convenience macro to iterate all gauge fields
#define STATS_GAUGE_FIELDS(X) STATS_QUIC_GAUGE_FIELDS(X) STATS_MOQ_GAUGE_FIELDS(X)

// Histograms: X(name, constexpr_bounds_ref, unit_suffix)
// unit_suffix is a string literal appended to the Prometheus metric name.
// Each expands to: name##Buckets[] (len = bounds.size()+1 for +Inf),
//                  name##Sum, name##Count
#define STATS_MOQ_HISTOGRAM_FIELDS(X)                                                              \
  X(moqSubscribeLatency, kLatencyBucketsUs, "microseconds")                                        \
  X(moqFetchLatency, kLatencyBucketsUs, "microseconds")                                            \
  X(moqPublishNamespaceLatency, kLatencyBucketsUs, "microseconds")                                 \
  X(moqPublishLatency, kLatencyBucketsUs, "microseconds")                                          \
  X(moqObjectAckLatency, kLatencyBucketsUs, "microseconds")

// QUIC transport histograms — populated by QuicStatsCollector and PicoQuicStatsCollector.
// Per-loop packet fields require an EventBaseStatsCollector loop observer to be wired up.
#define STATS_QUIC_HISTOGRAM_FIELDS(X)                                                             \
  X(quicRttSample, kRttBucketsMs, "milliseconds")                                                  \
  X(quicBandwidthSample, kBandwidthBucketsBitsPerSec, "bits_per_second")                           \
  X(quicInflightBytesSample, kInflightBytesBuckets, "bytes")                                       \
  X(quicCwndHintBytesSample, kInflightBytesBuckets, "bytes")                                       \
  X(evbPktsSentPerLoop, kEvbPktsPerLoopBuckets, "packets")                                         \
  X(evbPktsRecvPerLoop, kEvbPktsPerLoopBuckets, "packets")

// EventBase loop time histograms — populated by EventBaseStatsCollector, one per IO thread.
#define STATS_EVB_HISTOGRAM_FIELDS(X)                                                              \
  X(evbLoopBusy, kEvbLoopBucketsUs, "microseconds")                                                \
  X(evbLoopIdle, kEvbLoopBucketsUs, "microseconds")

#define STATS_HISTOGRAM_FIELDS(X)                                                                  \
  STATS_MOQ_HISTOGRAM_FIELDS(X) STATS_QUIC_HISTOGRAM_FIELDS(X) STATS_EVB_HISTOGRAM_FIELDS(X)

// Error-code breakdowns: fields in STATS_MOQ_COUNTER_FIELDS whose callbacks receive
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

// Reset-reason breakdowns: counters whose callback receives a
// ResetStreamErrorCode.  Each expands to a
// std::array<uint64_t, kResetStreamErrorCodeCount> named name##ByResetCodes.
#define STATS_RESET_COUNTER_FIELDS(X)                                                              \
  X(pubSubgroupReset)                                                                              \
  X(subSubgroupReset)

struct StatsSnapshot {
  // --- Scalar fields ---
#define DEFINE_FIELD(type, name) type name{0};
  STATS_COUNTER_FIELDS(DEFINE_FIELD)
  STATS_GAUGE_FIELDS(DEFINE_FIELD)
#undef DEFINE_FIELD

  // --- Histogram fields ---
#define DEFINE_HISTOGRAM(name, bounds, unit)                                                       \
  std::array<uint64_t, std::tuple_size_v<decltype(bounds)> + 1> name##Buckets{};                   \
  uint64_t name##Sum{0};                                                                           \
  uint64_t name##Count{0};
  STATS_HISTOGRAM_FIELDS(DEFINE_HISTOGRAM)
#undef DEFINE_HISTOGRAM

  // --- Per-RequestErrorCode breakdown arrays ---
#define DEFINE_ERROR_ARRAY(name) std::array<uint64_t, kRequestErrorCodeCount> name##ByCodes{};
  STATS_ERROR_COUNTER_FIELDS(DEFINE_ERROR_ARRAY)
#undef DEFINE_ERROR_ARRAY

  // --- Per-ResetStreamErrorCode breakdown arrays ---
#define DEFINE_RESET_ARRAY(name)                                                                   \
  std::array<uint64_t, kResetStreamErrorCodeCount> name##ByResetCodes{};
  STATS_RESET_COUNTER_FIELDS(DEFINE_RESET_ARRAY)
#undef DEFINE_RESET_ARRAY

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
  // Raw pointer; lifetime guaranteed by owning thread loop (e.g. EventBase).
  virtual folly::Executor* owningExecutor() const = 0;
};

class EventBaseStatsCollector;

class StatsRegistry {
public:
  StatsRegistry() = default;
  ~StatsRegistry() = default;

  void registerCollector(std::shared_ptr<StatsCollectorBase> collector);

  // Registers the collector for aggregation and associates it with its EVB so
  // QUIC collectors can subscribe to loop samples.  May be called from any
  // thread; the EVB association is scheduled onto the EVB thread.
  void
  registerEvbCollector(folly::EventBase* evb, std::shared_ptr<EventBaseStatsCollector> collector);

  // Must be called on the EVB thread.
  std::weak_ptr<EventBaseStatsCollector> findEvbCollector(folly::EventBase* evb) const;

  folly::coro::Task<StatsSnapshot> aggregateAsync();

private:
  std::vector<std::weak_ptr<StatsCollectorBase>> collectors_;
  mutable folly::EventBaseLocal<std::weak_ptr<EventBaseStatsCollector>> evbCollectors_;
};

} // namespace openmoq::moqx::stats
