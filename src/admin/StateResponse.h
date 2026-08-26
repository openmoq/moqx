/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <memory>

#include <folly/CancellationToken.h>
#include <folly/coro/AsyncGenerator.h>
#include <folly/coro/Task.h>
#include <folly/io/IOBuf.h>
#include <proxygen/httpserver/ResponseHandler.h>

#include "admin/EgressGate.h"

namespace openmoq::moqx::admin {

// The /state walk cannot be slowed down -- it is synchronous and never yields
// its executor -- so a consumer that stops draining cannot push back. This
// bounds what that can cost instead: past the cap the response is abandoned.
// Only our own queue counts against it; bytes handed to proxygen are released.
inline constexpr size_t kMaxBufferedStateBytes = 4 * 1024 * 1024;

// Bytes handed to the pipe but not yet handed to proxygen.
class StreamBudget {
public:
  explicit StreamBudget(size_t cap = kMaxBufferedStateBytes) : cap_(cap) {}

  bool tryAdd(size_t n) { return outstanding_.fetch_add(n, std::memory_order_relaxed) + n <= cap_; }
  void release(size_t n) { outstanding_.fetch_sub(n, std::memory_order_relaxed); }

private:
  std::atomic<size_t> outstanding_{0};
  size_t cap_;
};

// Drains a chunk stream into a chunked response on the admin EVB. Headers wait
// for the first chunk, so a walk that fails before producing any output -- the
// usual case, since a small /state does not flush until the end -- can still be
// reported as a 500 rather than an aborted 200.
folly::coro::Task<void> sendState(
    proxygen::ResponseHandler* ds,
    folly::CancellationToken token,
    folly::coro::AsyncGenerator<std::unique_ptr<folly::IOBuf>&&> gen,
    std::shared_ptr<StreamBudget> budget,
    std::shared_ptr<EgressGate> egress
);

} // namespace openmoq::moqx::admin
