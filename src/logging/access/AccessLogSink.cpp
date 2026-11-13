/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "logging/access/AccessLogSink.h"

#include <algorithm>
#include <optional>

namespace openmoq::moqx::logging {

namespace {

constexpr std::chrono::milliseconds kMinRetry{1000};
constexpr std::chrono::milliseconds kMaxRetry{30000};
// A sink in retry keeps at most this many batches. Older lines are dropped and show up
// as seq gaps.
constexpr size_t kMaxRetainedBatches = 4;

} // namespace

AccessLogSink::AccessLogSink(
    AccessLogSinkOptions options,
    TemplateEncoder encoder,
    std::unique_ptr<AccessLogTransport> transport
)
    : options_(std::move(options)), encoder_(std::move(encoder)), transport_(std::move(transport)) {
}

void AccessLogSink::append(
    const AccessLogRecord& record,
    const AccessLogRelayInfo& relayInfo,
    uint64_t queueDropped
) {
  // While the sink waits to retry, the drop count grows. The first line after a successful
  // write reports it.
  bool waiting = retryAt_ != std::chrono::steady_clock::time_point{};
  if (!waiting) {
    reportDrops(relayInfo, queueDropped);
  }
  encodeLine(record, relayInfo);
  if (waiting) {
    dropBacklog();
  }
}

void AccessLogSink::reportDrops(const AccessLogRelayInfo& relayInfo, uint64_t queueDropped) {
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
  encodeLine(drop, relayInfo);
}

void AccessLogSink::encodeLine(const AccessLogRecord& record, const AccessLogRelayInfo& relayInfo) {
  scratch_.clear();
  encoder_.encode(record, AccessLogRenderContext{relayInfo, nextSeq_++}, scratch_);
  scratch_.push_back('\n');
  pendingLines_.append(scratch_);
  lines_.push_back({scratch_.size(), record.dropped.value_or(0)});
}

void AccessLogSink::consume(size_t bytes) {
  pendingLines_.trimStart(bytes);
  while (bytes && !lines_.empty() && bytes >= lines_.front().length) {
    bytes -= lines_.front().length;
    lines_.pop_front();
    stats_.records++;
    firstLinePartial_ = false;
  }
  if (bytes) {
    lines_.front().length -= bytes;
    firstLinePartial_ = true;
  }
}

void AccessLogSink::dropBacklog() {
  auto limit = std::max<size_t>(options_.batchRecords, 1) * kMaxRetainedBatches;
  if (lines_.size() <= limit) {
    return;
  }
  // The oldest lines go first, except a partly written first line. Its remaining bytes
  // must be the next ones written, or the file gets half a line.
  std::optional<Line> partial;
  if (firstLinePartial_) {
    partial = lines_.front();
    lines_.pop_front();
  }
  auto excess = lines_.size() + (partial ? 1 : 0) - limit;
  size_t dropBytes = 0;
  for (size_t i = 0; i < excess; ++i) {
    dropBytes += lines_.front().length;
    // The next LOG_DROPPED line includes the count of a dropped one.
    lostSinceReport_ += lines_.front().dropped ? lines_.front().dropped : 1;
    lines_.pop_front();
  }
  // Remove the dropped bytes from between the partial line and the lines that stay.
  std::unique_ptr<folly::IOBuf> head;
  if (partial) {
    head = pendingLines_.split(partial->length);
    lines_.push_front(*partial);
  }
  pendingLines_.trimStart(dropBytes);
  if (head) {
    auto rest = pendingLines_.move();
    pendingLines_.append(std::move(head));
    pendingLines_.append(std::move(rest));
  }
  stats_.dropped += excess;
}

void AccessLogSink::clearPending() {
  pendingLines_.move();
  lines_.clear();
  firstLinePartial_ = false;
}

folly::coro::Task<void> AccessLogSink::flush() {
  if (lines_.empty() || std::chrono::steady_clock::now() < retryAt_) {
    co_return;
  }
  auto result = co_await transport_->write(*pendingLines_.front());
  switch (result.status) {
  case AccessLogWriteStatus::Ok:
    stats_.records += lines_.size();
    clearPending();
    retryAt_ = {};
    retryDelay_ = {};
    break;
  case AccessLogWriteStatus::Retry:
    stats_.writeErrors++;
    consume(result.bytesWritten);
    retryDelay_ = std::min(std::max({result.retryAfter, kMinRetry, retryDelay_ * 2}), kMaxRetry);
    retryAt_ = std::chrono::steady_clock::now() + retryDelay_;
    dropBacklog();
    break;
  }
}

void AccessLogSink::reopen() {
  transport_->reopen();
}

folly::coro::Task<void>
AccessLogSink::close(const AccessLogRelayInfo& relayInfo, uint64_t queueDropped) {
  reportDrops(relayInfo, queueDropped);
  retryAt_ = {};
  // A cancelled flush leaves its lines pending, and they count as dropped below.
  co_await folly::coro::co_awaitTry(flush());
  stats_.dropped += lines_.size();
  clearPending();
  co_await transport_->close();
}

} // namespace openmoq::moqx::logging
