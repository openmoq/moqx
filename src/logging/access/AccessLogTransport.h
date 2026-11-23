/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <cstddef>

#include <folly/coro/Task.h>
#include <folly/io/IOBuf.h>

namespace openmoq::moqx::logging {

enum class AccessLogWriteStatus { Ok, Retry };

struct AccessLogWriteResult {
  AccessLogWriteStatus status{AccessLogWriteStatus::Ok};
  std::chrono::milliseconds retryAfter{0};
  // On Retry, the bytes sent before the failure. The next write starts after them.
  size_t bytesWritten{0};
};

// Writes framed lines. Runs on the access log's executor. During stop(), a write that
// suspends must complete promptly when its cancellation token fires at the deadline.
class AccessLogTransport {
public:
  virtual ~AccessLogTransport() = default;
  virtual folly::coro::Task<AccessLogWriteResult> write(const folly::IOBuf& data) = 0;
  virtual void reopen() {}
  virtual folly::coro::Task<void> close() = 0;
};

} // namespace openmoq::moqx::logging
