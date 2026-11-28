/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <memory>
#include <string>

#include <folly/coro/Task.h>
#include <quic/common/CircularDeque.h>

#include "logging/access/AccessLogTransport.h"
#include "logging/access/AccessLogVars.h"

namespace openmoq::moqx::logging {

struct AccessLogSinkOptions {
  std::string name;
  size_t batchRecords{256};
  std::chrono::milliseconds flushInterval{1000};
};

// Written and read on the access log's executor only.
struct AccessLogSinkStats {
  uint64_t records{0};
  // Lines this sink encoded and then lost: retry backlog or shutdown.
  uint64_t dropped{0};
  uint64_t writeErrors{0};
};

// What one writePending() call wrote.
struct AccessLogSinkWriteResult {
  AccessLogWriteStatus status{AccessLogWriteStatus::Ok};
  std::chrono::milliseconds retryAfter{0};
  // The oldest pending records written in full, which the sink has removed.
  size_t records{0};
  // The oldest record still pending is partly written.
  bool partial{false};
  // The write sent something, so the output is draining.
  bool progressed{false};
};

// One named output. The access log owns the sink and calls it on its executor. This class
// numbers records, reports drops, limits the backlog and schedules writes. A subclass keeps
// the pending records and writes them.
class AccessLogSink {
public:
  AccessLogSink(AccessLogSinkOptions options, std::shared_ptr<const AccessLogRelayInfo> relayInfo);
  virtual ~AccessLogSink() = default;

  const AccessLogSinkOptions& options() const { return options_; }
  const AccessLogSinkStats& stats() const { return stats_; }
  const AccessLogRelayInfo& relayInfo() const { return *relayInfo_; }

  // Assigns seq and stores the record. queueDropped is the access log's running count of
  // records it dropped. If anything was lost since this sink's last LOG_DROPPED record and
  // the sink is not waiting to retry, it stores another first.
  void append(const AccessLogRecord& record, uint64_t queueDropped);

  bool batchFull() const { return pending_.size() >= options_.batchRecords; }
  bool hasPending() const { return !pending_.empty(); }
  // flush() does not write before this time.
  std::chrono::steady_clock::time_point retryAt() const { return retryAt_; }
  // A retrying sink flushes at retryAt(), whatever its batch size.
  bool retrying() const { return retryAt_ != std::chrono::steady_clock::time_point{}; }

  folly::coro::Task<AccessLogWriteStatus> flush();
  virtual void reopen() {}
  // Stores a final LOG_DROPPED record if needed, flushes until the output drains or a write
  // fails, counts what is left as dropped, and closes the output.
  folly::coro::Task<void> close(uint64_t queueDropped);

protected:
  // Adds a record after the pending ones. A string_view value is valid only during the call.
  virtual void addRecord(const AccessLogRecord& record, const AccessLogRenderContext& ctx) = 0;
  // Writes pending records, oldest first. Ok with records left asks to be called again soon.
  virtual folly::coro::Task<AccessLogSinkWriteResult> writePending() = 0;
  // Removes the oldest count records, skipping a partly written one.
  virtual void dropOldest(size_t count) = 0;
  // Discards the pending records and closes the output.
  virtual folly::coro::Task<void> closeOutput() = 0;

private:
  void reportDrops(uint64_t queueDropped);
  void store(const AccessLogRecord& record);
  void dropBacklog();

  AccessLogSinkOptions options_;
  std::shared_ptr<const AccessLogRelayInfo> relayInfo_;
  AccessLogSinkStats stats_;
  uint64_t queueDropsReported_{0};
  uint64_t lostSinceReport_{0};
  uint64_t nextSeq_{1};
  // One entry per pending record: the count that a LOG_DROPPED record carries, else 0.
  quic::CircularDeque<uint64_t> pending_;
  // From the last write: the oldest pending record is partly written.
  bool partial_{false};
  std::chrono::steady_clock::time_point retryAt_{};
  std::chrono::milliseconds retryDelay_{0};
};

} // namespace openmoq::moqx::logging
