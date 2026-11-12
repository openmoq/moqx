/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <folly/coro/Task.h>
#include <folly/io/IOBufQueue.h>
#include <quic/common/CircularDeque.h>

#include "logging/access/AccessLogTransport.h"
#include "logging/access/TemplateEncoder.h"

namespace openmoq::moqx::logging {

// Which records a sink takes. Event and class names come from the config.
struct AccessLogSinkFilter {
  // Indexed by AccessLogEvent; true where the sink takes the event.
  std::array<bool, size_t(AccessLogEvent::LOG_DROPPED) + 1> events{};
  bool errorsOnly{false};
  std::vector<std::string> services;
  std::vector<std::string> peerClasses;
  double sessionSample{1.0};
};

struct AccessLogSinkOptions {
  std::string name;
  AccessLogSinkFilter filter;
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

// One named output: filter, encoder and transport. The access log owns the sink and calls
// it on its executor.
class AccessLogSink {
public:
  AccessLogSink(
      AccessLogSinkOptions options,
      TemplateEncoder encoder,
      std::unique_ptr<AccessLogTransport> transport
  );

  const AccessLogSinkOptions& options() const { return options_; }
  const AccessLogSinkStats& stats() const { return stats_; }

  // Assigns seq and encodes into the pending lines. queueDropped is the access log's
  // running count of records it dropped. If anything was lost since this sink's last
  // LOG_DROPPED line and the sink is not waiting to retry, it writes another first.
  void
  append(const AccessLogRecord& record, const AccessLogRelayInfo& relayInfo, uint64_t queueDropped);

  bool batchFull() const { return lines_.size() >= options_.batchRecords; }
  bool hasPending() const { return !lines_.empty(); }
  // flush() does not write before this time.
  std::chrono::steady_clock::time_point retryAt() const { return retryAt_; }

  folly::coro::Task<void> flush();
  void reopen();
  // Writes a final LOG_DROPPED line if needed, flushes once, counts what is left as
  // dropped, and closes the transport.
  folly::coro::Task<void> close(const AccessLogRelayInfo& relayInfo, uint64_t queueDropped);

private:
  void reportDrops(const AccessLogRelayInfo& relayInfo, uint64_t queueDropped);
  void encodeLine(const AccessLogRecord& record, const AccessLogRelayInfo& relayInfo);
  // Removes the bytes a partial write sent.
  void consume(size_t bytes);
  void dropBacklog();
  void clearPending();

  AccessLogSinkOptions options_;
  TemplateEncoder encoder_;
  std::unique_ptr<AccessLogTransport> transport_;
  AccessLogSinkStats stats_;
  uint64_t queueDropsReported_{0};
  uint64_t lostSinceReport_{0};
  uint64_t nextSeq_{1};
  std::string scratch_;
  struct Line {
    // Excludes any bytes that a partial write already sent.
    size_t length{0};
    // The count that a LOG_DROPPED line carries.
    uint64_t dropped{0};
  };
  // The lines waiting to be written, with their newlines.
  folly::IOBufQueue pendingLines_;
  quic::CircularDeque<Line> lines_;
  bool firstLinePartial_{false};
  std::chrono::steady_clock::time_point retryAt_{};
  std::chrono::milliseconds retryDelay_{0};
};

} // namespace openmoq::moqx::logging
