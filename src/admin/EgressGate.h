/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>

#include <folly/coro/Task.h>
#include <folly/io/async/EventBase.h>
#include <proxygen/lib/http/coro/util/TimedBaton.h>

namespace openmoq::moqx::admin {

// Makes proxygen's egress pause/resume callbacks awaitable, so a route that
// streams a response can stop producing while the transaction cannot write --
// otherwise pausing just relocates the bytes into proxygen's egress queue
// instead of bounding them.
//
// Single-threaded, admin EVB only; the baton XCHECKs that.
class EgressGate {
public:
  // No timeout of our own: proxygen's transaction timeout already bounds a
  // stalled write, and a shorter deadline would abandon a merely slow client.
  explicit EgressGate(folly::EventBase* evb) : baton_(evb, std::chrono::milliseconds(0)) {}

  void onPaused() {
    if (done_ || paused_) {
      return;
    }
    paused_ = true;
    // Signalled state is sticky until reset, so a resume that beats the waiter
    // to the EVB is not a lost wakeup.
    baton_.reset();
  }

  void onResumed() {
    if (!paused_) {
      return;
    }
    paused_ = false;
    baton_.signal();
  }

  // The request is going away; wake any waiter and never park again.
  void shutdown() {
    done_ = true;
    paused_ = false;
    baton_.signal(proxygen::coro::TimedBaton::Status::cancelled);
  }

  bool paused() const { return paused_; }

  // False means the request is going away, so stop rather than send.
  folly::coro::Task<bool> awaitDrainable() {
    while (paused_) {
      const auto status = co_await baton_.wait();
      if (done_ || status == proxygen::coro::TimedBaton::Status::cancelled ||
          status == proxygen::coro::TimedBaton::Status::timedout) {
        co_return false;
      }
      // Anything else means a pause re-armed the baton before this waiter got
      // to run. paused_ is the truth; the status only says why we woke.
    }
    co_return true;
  }

private:
  proxygen::coro::TimedBaton baton_;
  bool paused_{false};
  bool done_{false};
};

} // namespace openmoq::moqx::admin
