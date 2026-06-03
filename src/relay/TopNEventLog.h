/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/MoQSession.h>
#include <moxygen/MoQTypes.h>

#include <chrono>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace openmoq::moqx {

/**
 * TopNEventLog - Structured event logger for SimpleTopNTracker operations.
 *
 * Emits TOPN_EVENT:{json} lines compatible with tools/topn_viz.py and
 * moq-rs's topn-log-to-html visualization format.
 *
 * Events:
 *   track_registered   - A new track is registered for ranking
 *   value_updated      - A track's property value changed
 *   track_removed      - A track was removed (publish done or idle sweep)
 *   subscriber_registered - A subscriber joined a top-N group
 *   top_n_selected     - A track was selected into a subscriber's top-N
 *   top_n_evicted      - A track was evicted from a subscriber's top-N
 *
 * Thread safety: all methods are thread-safe (internally synchronized).
 *
 * Enable via gflag --topn_event_log=<path>. When empty (default), all
 * methods are no-ops with zero overhead on the hot path.
 */
class TopNEventLog {
public:
  static TopNEventLog& instance();

  void init(const std::string& path);
  void shutdown();

  bool enabled() const { return enabled_; }

  void logTrackRegistered(
      const moxygen::FullTrackName& ftn,
      uint64_t initialValue,
      const moxygen::MoQSession* publisher);

  void logValueUpdated(
      const moxygen::FullTrackName& ftn,
      uint64_t oldValue,
      uint64_t newValue,
      const moxygen::MoQSession* publisher);

  void logTrackRemoved(const moxygen::FullTrackName& ftn);

  void logSubscriberRegistered(
      const moxygen::MoQSession* session, uint64_t maxSelected);

  void logTopNSelected(
      const moxygen::FullTrackName& ftn,
      const moxygen::MoQSession* session);

  void logTopNEvicted(
      const moxygen::FullTrackName& ftn,
      const moxygen::MoQSession* session);

private:
  TopNEventLog() = default;

  uint64_t elapsedMs() const;
  std::string trackStr(const moxygen::FullTrackName& ftn) const;
  uintptr_t sessionId(const moxygen::MoQSession* s) const;

  std::mutex mu_;
  std::ofstream file_;
  bool enabled_{false};
  std::chrono::steady_clock::time_point startTime_;
};

} // namespace openmoq::moqx
