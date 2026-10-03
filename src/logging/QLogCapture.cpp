/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "logging/QLogCapture.h"

#include <algorithm>

#include <folly/logging/xlog.h>

namespace openmoq::moqx::logging {

void QLogCapture::arm(uint32_t count, std::chrono::seconds duration, Mode mode) {
  count = std::min(count, kMaxCount);
  duration = std::min(duration, kMaxDuration);
  std::lock_guard lock(mutex_);
  mode_ = mode;
  remaining_ = count;
  captured_ = 0;
  deadline_ = std::chrono::steady_clock::now() + duration;
  expiresAt_ = std::chrono::system_clock::now() + duration;
  armed_.store(count > 0, std::memory_order_release);
  XLOG(INFO) << "qlog capture armed: next " << count << " connection(s) within " << duration.count()
             << "s, mode=" << modeName(mode);
}

void QLogCapture::disarm() {
  std::lock_guard lock(mutex_);
  if (armed_.load(std::memory_order_relaxed)) {
    XLOG(INFO) << "qlog capture disarmed after " << captured_ << " connection(s)";
  }
  remaining_ = 0;
  armed_.store(false, std::memory_order_release);
}

std::optional<QLogCapture::Mode> QLogCapture::take(std::chrono::steady_clock::time_point now) {
  if (!armed_.load(std::memory_order_acquire)) {
    return std::nullopt;
  }
  std::lock_guard lock(mutex_);
  if (remaining_ == 0 || now >= deadline_) {
    remaining_ = 0;
    armed_.store(false, std::memory_order_release);
    return std::nullopt;
  }
  --remaining_;
  ++captured_;
  if (remaining_ == 0) {
    armed_.store(false, std::memory_order_release);
  }
  return mode_;
}

QLogCapture::Status QLogCapture::status(std::chrono::steady_clock::time_point now) const {
  std::lock_guard lock(mutex_);
  const bool live = remaining_ > 0 && now < deadline_;
  return Status{
      .armed = live,
      .mode = mode_,
      .remaining = live ? remaining_ : 0,
      .captured = captured_,
      .expiresAt = expiresAt_,
  };
}

std::string_view QLogCapture::modeName(Mode mode) {
  return mode == Mode::Full ? "full" : "cc";
}

std::optional<QLogCapture::Mode> QLogCapture::parseMode(std::string_view name) {
  if (name == "full") {
    return Mode::Full;
  }
  if (name == "cc") {
    return Mode::Cc;
  }
  return std::nullopt;
}

} // namespace openmoq::moqx::logging
