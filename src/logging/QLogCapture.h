/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

#include <folly/Synchronized.h>

namespace openmoq::moqx::logging {

/**
 * On-demand qlog capture: qlog the next `count` new mvfst connections, until a
 * deadline. Shared by all mvfst listeners; take() is called from IO threads.
 */
class QLogCapture {
public:
  enum class Mode {
    Full, // every event qlog emits
    Cc,   // congestion control and recovery only; no per-packet or per-stream events
  };

  static constexpr uint32_t kMaxCount = 64;
  static constexpr std::chrono::seconds kMaxDuration{600};

  struct Status {
    bool armed{false};
    Mode mode{Mode::Cc};
    uint32_t remaining{0};
    uint32_t captured{0};
    std::chrono::system_clock::time_point expiresAt;
  };

  // Returns false, changing nothing, if a capture is in progress and !replace.
  // count and duration are clamped to the maxima.
  bool
  arm(uint32_t count,
      std::chrono::seconds duration,
      Mode mode,
      bool replace,
      std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
  void disarm();

  // Claims one connection if armed and not expired; returns the mode to log it with.
  std::optional<Mode>
  take(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

  Status status(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const;

  static std::string_view modeName(Mode mode);
  static std::optional<Mode> parseMode(std::string_view name);

private:
  struct State {
    Mode mode{Mode::Cc};
    uint32_t remaining{0};
    uint32_t captured{0};
    std::chrono::steady_clock::time_point deadline;
    std::chrono::system_clock::time_point expiresAt;

    bool live(std::chrono::steady_clock::time_point now) const {
      return remaining > 0 && now < deadline;
    }
  };

  folly::Synchronized<State> state_;
  // Lets take() skip the lock when no capture is armed. Written under the lock.
  std::atomic<bool> armed_{false};
};

} // namespace openmoq::moqx::logging
