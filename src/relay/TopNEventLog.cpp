/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/TopNEventLog.h"

#include <folly/logging/xlog.h>

namespace openmoq::moqx {

TopNEventLog& TopNEventLog::instance() {
  static TopNEventLog inst;
  return inst;
}

void TopNEventLog::init(const std::string& path) {
  std::lock_guard<std::mutex> lock(mu_);
  if (path.empty()) {
    return;
  }
  file_.open(path, std::ios::out | std::ios::trunc);
  if (file_.is_open()) {
    enabled_ = true;
    startTime_ = std::chrono::steady_clock::now();
    XLOG(INFO) << "[TopNEventLog] Logging to " << path;
  } else {
    XLOG(ERR) << "[TopNEventLog] Failed to open " << path;
  }
}

void TopNEventLog::shutdown() {
  std::lock_guard<std::mutex> lock(mu_);
  if (enabled_) {
    file_.flush();
    file_.close();
    enabled_ = false;
  }
}

void TopNEventLog::logTrackRegistered(
    const moxygen::FullTrackName& ftn,
    uint64_t initialValue,
    const moxygen::MoQSession* publisher) {
  if (!enabled_) return;
  std::lock_guard<std::mutex> lock(mu_);
  file_ << "TOPN_EVENT:{\"event\":\"track_registered\",\"ts_ms\":" << elapsedMs()
        << ",\"track\":\"" << trackStr(ftn) << "\""
        << ",\"initial_value\":" << initialValue
        << ",\"publisher_id\":" << sessionId(publisher) << "}\n";
}

void TopNEventLog::logValueUpdated(
    const moxygen::FullTrackName& ftn,
    uint64_t oldValue,
    uint64_t newValue,
    const moxygen::MoQSession* publisher) {
  if (!enabled_) return;
  std::lock_guard<std::mutex> lock(mu_);
  file_ << "TOPN_EVENT:{\"event\":\"value_updated\",\"ts_ms\":" << elapsedMs()
        << ",\"track\":\"" << trackStr(ftn) << "\""
        << ",\"old_value\":" << oldValue
        << ",\"new_value\":" << newValue
        << ",\"publisher_id\":" << sessionId(publisher) << "}\n";
}

void TopNEventLog::logTrackRemoved(const moxygen::FullTrackName& ftn) {
  if (!enabled_) return;
  std::lock_guard<std::mutex> lock(mu_);
  file_ << "TOPN_EVENT:{\"event\":\"track_removed\",\"ts_ms\":" << elapsedMs()
        << ",\"track\":\"" << trackStr(ftn) << "\"}\n";
}

void TopNEventLog::logSubscriberRegistered(
    const moxygen::MoQSession* session, uint64_t maxSelected) {
  if (!enabled_) return;
  std::lock_guard<std::mutex> lock(mu_);
  file_ << "TOPN_EVENT:{\"event\":\"subscriber_registered\",\"ts_ms\":" << elapsedMs()
        << ",\"subscriber_id\":" << sessionId(session)
        << ",\"n\":" << maxSelected << "}\n";
}

void TopNEventLog::logTopNSelected(
    const moxygen::FullTrackName& ftn,
    const moxygen::MoQSession* session) {
  if (!enabled_) return;
  std::lock_guard<std::mutex> lock(mu_);
  file_ << "TOPN_EVENT:{\"event\":\"top_n_selected\",\"ts_ms\":" << elapsedMs()
        << ",\"track\":\"" << trackStr(ftn) << "\""
        << ",\"subscriber_id\":" << sessionId(session) << "}\n";
}

void TopNEventLog::logTopNEvicted(
    const moxygen::FullTrackName& ftn,
    const moxygen::MoQSession* session) {
  if (!enabled_) return;
  std::lock_guard<std::mutex> lock(mu_);
  file_ << "TOPN_EVENT:{\"event\":\"top_n_evicted\",\"ts_ms\":" << elapsedMs()
        << ",\"track\":\"" << trackStr(ftn) << "\""
        << ",\"subscriber_id\":" << sessionId(session) << "}\n";
}

uint64_t TopNEventLog::elapsedMs() const {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - startTime_)
          .count());
}

std::string TopNEventLog::trackStr(const moxygen::FullTrackName& ftn) const {
  return ftn.trackName;
}

uintptr_t TopNEventLog::sessionId(const moxygen::MoQSession* s) const {
  return reinterpret_cast<uintptr_t>(s);
}

} // namespace openmoq::moqx
