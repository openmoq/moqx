/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "logging/QLogCapture.h"

#include <algorithm>

#include <folly/logging/xlog.h>

namespace openmoq::moqx::logging {

bool QLogCapture::arm(
    uint32_t count,
    std::chrono::seconds duration,
    Mode mode,
    bool replace,
    std::chrono::steady_clock::time_point now
) {
  count = std::min(count, kMaxCount);
  duration = std::min(duration, kMaxDuration);
  auto state = state_.wlock();
  if (!replace && state->live(now)) {
    return false;
  }
  state->mode = mode;
  state->remaining = count;
  state->captured = 0;
  state->deadline = now + duration;
  state->expiresAt = std::chrono::system_clock::now() + duration;
  armed_.store(count > 0, std::memory_order_release);
  XLOG(INFO) << "qlog capture armed: next " << count << " connection(s) within " << duration.count()
             << "s, mode=" << modeName(mode);
  return true;
}

void QLogCapture::disarm() {
  auto state = state_.wlock();
  if (armed_.load(std::memory_order_relaxed)) {
    XLOG(INFO) << "qlog capture disarmed after " << state->captured << " connection(s)";
  }
  state->remaining = 0;
  armed_.store(false, std::memory_order_release);
}

std::optional<QLogCapture::Mode> QLogCapture::take(std::chrono::steady_clock::time_point now) {
  if (!armed_.load(std::memory_order_acquire)) {
    return std::nullopt;
  }
  auto state = state_.wlock();
  if (!state->live(now)) {
    state->remaining = 0;
    armed_.store(false, std::memory_order_release);
    return std::nullopt;
  }
  --state->remaining;
  ++state->captured;
  if (state->remaining == 0) {
    armed_.store(false, std::memory_order_release);
  }
  return state->mode;
}

QLogCapture::Status QLogCapture::status(std::chrono::steady_clock::time_point now) const {
  auto state = state_.rlock();
  const bool live = state->live(now);
  return Status{
      .armed = live,
      .mode = state->mode,
      .remaining = live ? state->remaining : 0,
      .captured = state->captured,
      .expiresAt = state->expiresAt,
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
