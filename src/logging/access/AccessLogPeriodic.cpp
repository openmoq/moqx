/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "logging/access/AccessLogPeriodic.h"

#include <algorithm>
#include <map>

#include <folly/io/async/EventBaseLocal.h>
#include <folly/logging/xlog.h>

#include "logging/access/AccessLogRequest.h"

namespace openmoq::moqx::logging {

std::chrono::system_clock::time_point
nextMultiple(std::chrono::system_clock::time_point now, std::chrono::milliseconds interval) {
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
  return std::chrono::system_clock::time_point(ms - ms % interval + interval);
}

AccessLogPeriodic&
AccessLogPeriodic::on(folly::EventBase& evb, std::chrono::milliseconds interval) {
  using Timers = std::map<std::chrono::milliseconds, std::unique_ptr<AccessLogPeriodic>>;
  static folly::EventBaseLocal<Timers> local;
  XDCHECK(evb.isInEventBaseThread());
  auto& timer = local.try_emplace(evb)[interval];
  if (!timer) {
    timer = std::make_unique<AccessLogPeriodic>(&evb, interval);
  }
  return *timer;
}

void AccessLogPeriodic::add(std::weak_ptr<AccessLogRequest> logRequest) {
  XDCHECK(evb_->isInEventBaseThread());
  requests_.push_back(std::move(logRequest));
  // A sweep in progress schedules the next boundary when it finishes.
  if (!isScheduled() && !isLoopCallbackScheduled()) {
    scheduleNext();
  }
}

void AccessLogPeriodic::timeoutExpired() noexcept {
  next_ = 0;
  end_ = requests_.size();
  evb_->runInLoop(this);
}

void AccessLogPeriodic::sweep() {
  auto end = std::min(end_, next_ + kPerLoop);
  for (; next_ < end; ++next_) {
    auto& weak = requests_[next_];
    if (auto logRequest = weak.lock(); !logRequest || !logRequest->writePeriodic()) {
      weak.reset();
    }
  }
  if (next_ < end_) {
    evb_->runInLoop(this);
    return;
  }
  std::erase_if(requests_, [](const auto& weak) { return weak.expired(); });
  if (!requests_.empty()) {
    scheduleNext();
  }
}

void AccessLogPeriodic::scheduleNext() {
  auto now = std::chrono::system_clock::now();
  auto next = boundary_ ? *boundary_ + interval_ : nextMultiple(now, interval_);
  // A wall-clock step restarts the boundaries from now. The timer can fire a little early, so a
  // wait up to two intervals is normal.
  if (next <= now || next - now > 2 * interval_) {
    next = nextMultiple(now, interval_);
  }
  boundary_ = next;
  scheduleTimeout(std::chrono::duration_cast<std::chrono::milliseconds>(next - now));
}

} // namespace openmoq::moqx::logging
