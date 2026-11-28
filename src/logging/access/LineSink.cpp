/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "logging/access/LineSink.h"

#include <optional>

namespace openmoq::moqx::logging {

LineSink::LineSink(
    AccessLogSinkOptions options,
    std::shared_ptr<const AccessLogRelayInfo> relayInfo,
    TemplateEncoder encoder,
    std::unique_ptr<AccessLogTransport> transport
)
    : AccessLogSink(std::move(options), std::move(relayInfo)), encoder_(std::move(encoder)),
      transport_(std::move(transport)) {}

void LineSink::addRecord(const AccessLogRecord& record, const AccessLogRenderContext& ctx) {
  scratch_.clear();
  encoder_.encode(record, ctx, scratch_);
  scratch_.push_back('\n');
  pendingLines_.append(scratch_);
  lengths_.push_back(scratch_.size());
}

size_t LineSink::consume(size_t bytes) {
  pendingLines_.trimStart(bytes);
  size_t lines = 0;
  while (bytes && !lengths_.empty() && bytes >= lengths_.front()) {
    bytes -= lengths_.front();
    lengths_.pop_front();
    ++lines;
    firstLinePartial_ = false;
  }
  if (bytes) {
    lengths_.front() -= bytes;
    firstLinePartial_ = true;
  }
  return lines;
}

void LineSink::dropOldest(size_t count) {
  // The oldest lines go first, except a partly written first line. Its remaining bytes
  // must be the next ones written, or the file gets half a line.
  std::optional<size_t> partial;
  if (firstLinePartial_) {
    partial = lengths_.front();
    lengths_.pop_front();
  }
  size_t dropBytes = 0;
  for (size_t i = 0; i < count; ++i) {
    dropBytes += lengths_.front();
    lengths_.pop_front();
  }
  // Remove the dropped bytes from between the partial line and the lines that stay.
  std::unique_ptr<folly::IOBuf> head;
  if (partial) {
    head = pendingLines_.split(*partial);
    lengths_.push_front(*partial);
  }
  pendingLines_.trimStart(dropBytes);
  if (head) {
    auto rest = pendingLines_.move();
    pendingLines_.append(std::move(head));
    pendingLines_.append(std::move(rest));
  }
}

void LineSink::clearPending() {
  pendingLines_.move();
  lengths_.clear();
  firstLinePartial_ = false;
}

folly::coro::Task<AccessLogSinkWriteResult> LineSink::writePending() {
  lineLengths_.assign(lengths_.begin(), lengths_.end());
  auto result = co_await transport_->write(*pendingLines_.front(), lineLengths_);
  AccessLogSinkWriteResult written{result.status, result.retryAfter};
  if (result.status == AccessLogWriteStatus::Ok) {
    written.records = lengths_.size();
    clearPending();
  } else {
    written.records = consume(result.bytesWritten);
    written.progressed = result.bytesWritten > 0;
  }
  written.partial = firstLinePartial_;
  if (reopenPending_ && !firstLinePartial_) {
    reopenPending_ = false;
    transport_->reopen();
  }
  co_return written;
}

void LineSink::reopen() {
  if (firstLinePartial_) {
    reopenPending_ = true;
    return;
  }
  transport_->reopen();
}

folly::coro::Task<void> LineSink::closeOutput() {
  clearPending();
  co_await transport_->close();
}

} // namespace openmoq::moqx::logging
