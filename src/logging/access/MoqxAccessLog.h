/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <folly/coro/UnboundedQueue.h>
#include <folly/futures/Future.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/EventBaseLocal.h>
#include <folly/io/async/ScopedEventBaseThread.h>

#include "logging/access/AccessLog.h"
#include "logging/access/AccessLogSink.h"

namespace openmoq::moqx::logging {

class AccessLogStatsVisitor {
public:
  virtual ~AccessLogStatsVisitor() = default;
  virtual void onSink(std::string_view name, const AccessLogSinkStats& stats) = 0;
  virtual void onQueue(size_t depth, uint64_t dropped, double oldestRecordAgeSeconds) = 0;
};

// Session threads submit typed records, and one coroutine on the moqx-accesslog thread
// encodes and writes them, so the lines from one session thread stay in order.
class MoqxAccessLog : public AccessLog {
public:
  MoqxAccessLog(
      AccessLogRelayInfo relayInfo,
      std::vector<std::unique_ptr<AccessLogSink>> sinks,
      size_t queueLimit
  );
  // Every EventBase that called submit() must be destroyed first. Their buffers hold a
  // reference to this, and destroying it does not destroy them.
  ~MoqxAccessLog() override;

  // A log without sinks does not start its thread, and start(), stop() and reopen() do
  // nothing.
  bool enabled() const override { return thread_.has_value(); }

  void start();
  // Writes what is queued until the deadline, counts the rest as dropped, then closes the
  // sinks. A write that blocks can still hold it past the deadline.
  void stop(std::chrono::steady_clock::time_point deadline);

  AccessLogSessionGate
  gateSession(std::string_view service, std::string_view peerClass, std::string_view sampleKey)
      const override;
  AccessLogSinkMask
  eventMask(const AccessLogSessionGate& gate, AccessLogEvent event, bool isError) const override;
  // Thread-safe. On an EventBase thread, records are queued together at the end of the
  // loop iteration, and the queue limit is checked once per batch. A record that arrives
  // after stop() is dropped and counted.
  void submit(std::unique_ptr<AccessLogRecord> record, AccessLogSinkMask mask) override;

  // Thread-safe. Each sink reopens its transport on the access log thread.
  void reopen();

  const AccessLogRelayInfo& relayInfo() const { return relayInfo_; }
  size_t queueDepth() const { return depth_.load(std::memory_order_relaxed); }
  uint64_t queueDropped() const { return queueDropped_.load(std::memory_order_relaxed); }

  // Calls the visitor on the access log thread. Valid before start() and after stop().
  folly::coro::Task<void> visitStats(AccessLogStatsVisitor& visitor);

private:
  enum class State : uint8_t { Created, Running, Stopped };

  struct Entry {
    std::unique_ptr<AccessLogRecord> record;
    AccessLogSinkMask mask{0};
  };

  enum class ItemKind : uint8_t { Records, Reopen, Stop };
  struct Item {
    ItemKind kind{ItemKind::Records};
    std::vector<Entry> entries;
  };

  // One EventBase's records for the current loop iteration.
  class ThreadBuffer : public folly::EventBase::LoopCallback {
  public:
    explicit ThreadBuffer(MoqxAccessLog& log) : log_(log) {}
    // Enqueues what is left, because an EventBase that is being destroyed can drop a
    // scheduled loop callback.
    ~ThreadBuffer() override;

    void add(folly::EventBase& evb, Entry entry);
    void runLoopCallback() noexcept override;

  private:
    MoqxAccessLog& log_;
    std::vector<Entry> entries_;
  };

  void enqueueBatch(std::vector<Entry> entries);
  folly::coro::Task<void> run();
  void handleRecords(const std::vector<Entry>& entries);
  void noteDropped(uint64_t count = 1);
  // When the earliest sink with pending lines is due to flush.
  std::optional<std::chrono::steady_clock::time_point> nextFlush() const;
  folly::coro::Task<void> flushDue();
  folly::coro::Task<void> closeSinks();
  folly::coro::Task<void> untilStopDeadline(folly::coro::Task<void> task);
  void reportStats(AccessLogStatsVisitor& visitor);

  AccessLogRelayInfo relayInfo_;
  std::vector<std::unique_ptr<AccessLogSink>> sinks_;
  // Indexed by AccessLogEvent: the sinks whose events list takes it.
  std::vector<AccessLogSinkMask> eventSinks_;
  AccessLogSinkMask errorsOnlySinks_{0};
  std::vector<std::chrono::steady_clock::time_point> lastFlush_;
  size_t queueLimit_;
  std::atomic<size_t> depth_{0};
  // The one counter that producers write: records they could not queue.
  std::atomic<uint64_t> queueDropped_{0};
  std::atomic<State> state_{State::Created};
  std::chrono::steady_clock::time_point stopDeadline_{};
  folly::coro::UnboundedQueue<Item, /*SingleProducer=*/false, /*SingleConsumer=*/true> queue_;
  std::optional<folly::ScopedEventBaseThread> thread_;
  folly::SemiFuture<folly::Unit> done_{folly::makeSemiFuture()};
  folly::EventBaseLocal<ThreadBuffer> buffers_;
};

// Maps a key (the session ID) to [0, 1). A sink samples the session in when this is below
// its rate, so a session sampled at one rate is also sampled at every higher rate.
double accessLogSampleKey(std::string_view key);

} // namespace openmoq::moqx::logging
