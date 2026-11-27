/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <vector>

#include <folly/io/async/AsyncTimeout.h>
#include <folly/io/async/EventBase.h>

namespace openmoq::moqx::logging {

class AccessLogRequest;

// Writes SUB_STATS for the sampled subscriptions of one EventBase that share an interval. The
// intervals end on wall-clock multiples of the interval, so lines from every thread and relay
// line up. A sweep writes at most kPerLoop lines in one loop iteration and continues in the next.
class AccessLogPeriodic : public folly::AsyncTimeout, public folly::EventBase::LoopCallback {
public:
  static constexpr size_t kPerLoop = 256;

  AccessLogPeriodic(folly::EventBase* evb, std::chrono::milliseconds interval)
      : folly::AsyncTimeout(evb), evb_(evb), interval_(interval) {}

  // The one for evb and interval. Runs on evb.
  static AccessLogPeriodic& on(folly::EventBase& evb, std::chrono::milliseconds interval);

  void add(std::weak_ptr<AccessLogRequest> logRequest);

  size_t size() const { return requests_.size(); }

  void timeoutExpired() noexcept override;
  void runLoopCallback() noexcept override { sweep(); }

private:
  void sweep();
  void scheduleNext();

  folly::EventBase* evb_;
  const std::chrono::milliseconds interval_;
  std::vector<std::weak_ptr<AccessLogRequest>> requests_;
  // Where the sweep in progress continues, and where it stops. A request added during the
  // sweep waits for the next one.
  size_t next_{0};
  size_t end_{0};
  // The boundary that the timer was last set for. The timer can fire a little early, so the
  // next boundary is this one plus the interval.
  std::optional<std::chrono::system_clock::time_point> boundary_;
};

// The first wall-clock multiple of interval after now.
std::chrono::system_clock::time_point
nextMultiple(std::chrono::system_clock::time_point now, std::chrono::milliseconds interval);

} // namespace openmoq::moqx::logging
