/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "logging/access/AccessLogSink.h"

#include <algorithm>
#include <optional>

#include <folly/coro/Sleep.h>
#include <folly/logging/xlog.h>

#include "relay/SessionConfined.h"

namespace openmoq::moqx::logging {

namespace {

constexpr std::chrono::milliseconds kMinRetry{1000};
constexpr std::chrono::milliseconds kMaxRetry{30000};
constexpr std::chrono::milliseconds kDrainRetry{10};
// A sink in retry keeps at most this many batches. Older lines are dropped and show up
// as seq gaps.
constexpr size_t kMaxRetainedBatches = 4;

} // namespace

AccessLogSink::AccessLogSink(
    AccessLogSinkOptions options,
    std::shared_ptr<const AccessLogRelayInfo> relayInfo
)
    : options_(std::move(options)), relayInfo_(std::move(relayInfo)) {
  XCHECK(relayInfo_);
}

void AccessLogSink::append(const AccessLogRecord& record, uint64_t queueDropped) {
  // While the sink waits to retry, the drop count grows. The first line after a successful
  // write reports it.
  bool waiting = retryAt_ != std::chrono::steady_clock::time_point{};
  if (!waiting) {
    reportDrops(queueDropped);
  }
  store(record);
  if (waiting) {
    dropBacklog();
  }
}

void AccessLogSink::reportDrops(uint64_t queueDropped) {
  // Every sink reports the queue's drops, because the queue does not keep the masks of the
  // records that it dropped.
  auto lost = queueDropped - queueDropsReported_ + lostSinceReport_;
  if (!lost) {
    return;
  }
  queueDropsReported_ = queueDropped;
  lostSinceReport_ = 0;
  AccessLogRecord drop;
  drop.event = AccessLogEvent::LOG_DROPPED;
  drop.time = std::chrono::system_clock::now();
  drop.dropped = lost;
  store(drop);
}

void AccessLogSink::store(const AccessLogRecord& record) {
  addRecord(record, AccessLogRenderContext{*relayInfo_, nextSeq_++});
  pending_.push_back(record.dropped.value_or(0));
}

void AccessLogSink::dropBacklog() {
  auto limit = std::max<size_t>(options_.batchRecords, 1) * kMaxRetainedBatches;
  if (pending_.size() <= limit) {
    return;
  }
  auto excess = pending_.size() - limit;
  // The sink keeps a partly written record, so its entry stays too.
  std::optional<uint64_t> kept;
  if (partial_) {
    kept = pending_.front();
    pending_.pop_front();
  }
  for (size_t i = 0; i < excess; ++i) {
    // The next LOG_DROPPED line includes the count of a dropped one.
    lostSinceReport_ += pending_.front() ? pending_.front() : 1;
    pending_.pop_front();
  }
  if (kept) {
    pending_.push_front(*kept);
  }
  dropOldest(excess);
  stats_.dropped += excess;
}

folly::coro::Task<AccessLogWriteStatus> AccessLogSink::flush() {
  if (pending_.empty() || std::chrono::steady_clock::now() < retryAt_) {
    co_return AccessLogWriteStatus::Ok;
  }
  auto result = co_await writePending();
  XDCHECK_LE(result.records, pending_.size());
  XDCHECK(result.status != AccessLogWriteStatus::Ok || result.records > 0);
  for (size_t i = 0; i < result.records; ++i) {
    pending_.pop_front();
  }
  partial_ = result.partial;
  stats_.records += result.records;
  switch (result.status) {
  case AccessLogWriteStatus::Ok:
    retryDelay_ = {};
    // A sink that sends part of its records per write is due again at once.
    retryAt_ = pending_.empty() ? std::chrono::steady_clock::time_point{}
                                : std::chrono::steady_clock::now();
    break;
  case AccessLogWriteStatus::Retry:
  case AccessLogWriteStatus::Failed:
    stats_.writeErrors++;
    if (result.status == AccessLogWriteStatus::Retry && result.progressed) {
      // A write that sent bytes shows that the reader is draining the output.
      retryDelay_ = {};
      retryAt_ = std::chrono::steady_clock::now() + kDrainRetry;
    } else {
      retryDelay_ = std::min(std::max({result.retryAfter, kMinRetry, retryDelay_ * 2}), kMaxRetry);
      retryAt_ = std::chrono::steady_clock::now() + retryDelay_;
    }
    dropBacklog();
    break;
  }
  co_return result.status;
}

folly::coro::Task<void> AccessLogSink::close(uint64_t queueDropped) {
  reportDrops(queueDropped);
  // Retry a full output until it drains or the stop deadline cancels the loop. A
  // cancelled flush leaves its lines pending, and they count as dropped below.
  while (!pending_.empty()) {
    retryAt_ = {};
    auto status = co_await folly::coro::co_awaitTry(flush());
    if (!status.hasValue() || *status == AccessLogWriteStatus::Failed || pending_.empty()) {
      break;
    }
    if (*status != AccessLogWriteStatus::Ok &&
        (co_await folly::coro::co_awaitTry(folly::coro::sleep(kDrainRetry))).hasException()) {
      break;
    }
  }
  stats_.dropped += pending_.size();
  pending_.clear();
  partial_ = false;
  co_await closeOutput();
}

} // namespace openmoq::moqx::logging
