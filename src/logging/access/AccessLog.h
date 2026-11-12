/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string_view>

#include "logging/access/AccessLogRecord.h"

namespace openmoq::moqx::logging {

// Bit i selects sink i.
using AccessLogSinkMask = uint64_t;
inline constexpr size_t kMaxAccessLogSinks = 64;

// The sinks one session's records may reach, decided once at SETUP.
struct AccessLogSessionGate {
  // Sinks whose services and peer_classes match the session. Errors reach these.
  AccessLogSinkMask matched{0};
  // The matched sinks that also sampled the session in.
  AccessLogSinkMask sampled{0};
};

// Where an observer sends records. MoqxAccessLog implements it, and tests capture
// records with their own implementation.
class AccessLog {
public:
  virtual ~AccessLog() = default;
  // False for a log without sinks.
  virtual bool enabled() const { return true; }
  virtual AccessLogSessionGate
  gateSession(std::string_view service, std::string_view peerClass, std::string_view sampleKey)
      const = 0;
  // The sinks that take this event. Zero means the caller should not build the record.
  virtual AccessLogSinkMask
  eventMask(const AccessLogSessionGate& gate, AccessLogEvent event, bool isError) const = 0;
  virtual void submit(std::unique_ptr<AccessLogRecord> record, AccessLogSinkMask mask) = 0;

  // After this, a request that ends gets end_reason "shutdown".
  void beginShutdown() { shuttingDown_.store(true, std::memory_order_relaxed); }
  bool shuttingDown() const { return shuttingDown_.load(std::memory_order_relaxed); }

private:
  std::atomic<bool> shuttingDown_{false};
};

} // namespace openmoq::moqx::logging
