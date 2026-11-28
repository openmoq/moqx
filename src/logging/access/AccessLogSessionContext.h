/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <folly/SocketAddress.h>

#include "logging/access/AccessLog.h"

namespace folly {
class EventBase;
}

namespace openmoq::moqx::logging {

class AccessLogRequest;

enum class AccessLogSessionState : uint8_t { Open, Closed, ShuttingDown };

// The session fields that CLIENT_SETUP can change. The service and peer class choose the sinks.
struct AccessLogSessionSetup {
  std::string service;
  std::string peerClass;
  std::optional<uint64_t> version;
  std::string authority;
  // With its query, which can carry tokens.
  std::string path;
};

// Reads a live session for the access log, on the session's EventBase.
class AccessLogSessionProbe {
public:
  virtual ~AccessLogSessionProbe() = default;
  // True once the session is closing or destroyed.
  virtual bool closed() const = 0;
  // Sets the record's transport fields once the transport has measured an RTT.
  virtual void addTransportStats(AccessLogRecord& record) const = 0;
};

// The session fields that do not change after setup(), and how to read the session.
struct AccessLogSessionSource {
  std::string sessionId;
  folly::SocketAddress peer;
  folly::SocketAddress local;
  // The upstream URL, on sessions the relay connected.
  std::string upstream;
  // Null when the session does not run on an EventBase, which turns off SUB_STATS.
  folly::EventBase* evb{nullptr};
  std::unique_ptr<AccessLogSessionProbe> probe;
};

// One session's access log state. All calls run on the session's executor.
class AccessLogSessionContext {
public:
  AccessLogSessionContext(
      std::shared_ptr<AccessLog> log,
      AccessLogOpenedBy openedBy,
      std::chrono::steady_clock::time_point openedAt = std::chrono::steady_clock::now()
  );

  // Picks the session's sinks and sets the fields that every record carries. A sink does
  // not take a record written before this.
  void setup(AccessLogSessionSource source, AccessLogSessionSetup setup);
  // At CLIENT_SETUP: replaces the AccessLogSessionSetup fields and picks the sinks again.
  void update(AccessLogSessionSetup setup);

  // Builds a record and calls setFields(record) only when a sink takes the event.
  template <typename SetFields>
  void write(AccessLogEvent event, bool isError, SetFields&& setFields) {
    writeFrom(event, isError, nullptr, setFields);
  }
  // The same, with the record starting as a copy of base.
  template <typename SetFields>
  void
  write(AccessLogEvent event, bool isError, const AccessLogRecord& base, SetFields&& setFields) {
    writeFrom(event, isError, &base, setFields);
  }
  // The same, to the sinks in mask.
  template <typename SetFields>
  void writeTo(
      AccessLogSinkMask mask,
      AccessLogEvent event,
      const AccessLogRecord& base,
      SetFields&& setFields
  ) {
    if (mask != 0) {
      submit(mask, event, &base, setFields);
    }
  }

  void writeSetup(
      AccessLogStatus status,
      std::optional<uint64_t> errorCode = std::nullopt,
      std::string reason = {}
  );
  // Writes SESSION_END once.
  void writeSessionEnd(std::optional<uint64_t> errorCode);
  void addTransportStats(AccessLogRecord& record) const;

  // ShuttingDown during relay shutdown, and Closed once the session is closing or
  // destroyed. close() sets the closed flag before it tears down requests, so after that
  // every end is teardown. Open before setup() and while the session is open.
  AccessLogSessionState state() const;

  // False when the session's service selects zero sinks for its peer class, so its requests
  // are not tracked.
  bool active() const { return gate_.matched != 0; }
  // True once setup() has found that the session reaches zero sinks.
  bool filteredOut() const { return probe_ && !active(); }

  void countRequest() { ++requests_; }

  // The sinks that take SUB_STATS for one of the session's subscriptions.
  AccessLogSinkMask periodicMask(uint64_t requestID) const;
  // Registers logRequest with the timer for its interval on the session's EventBase, which calls
  // writePeriodic() every interval. False when the session does not run on an EventBase.
  bool schedulePeriodic(std::weak_ptr<AccessLogRequest> logRequest);

private:
  template <typename SetFields>
  void
  writeFrom(AccessLogEvent event, bool isError, const AccessLogRecord* base, SetFields& setFields) {
    auto mask = log_->eventMask(gate_, event, isError);
    if (mask != 0) {
      submit(mask, event, base, setFields);
    }
  }

  template <typename SetFields>
  void submit(
      AccessLogSinkMask mask,
      AccessLogEvent event,
      const AccessLogRecord* base,
      SetFields& setFields
  ) {
    auto record =
        base ? std::make_unique<AccessLogRecord>(*base) : std::make_unique<AccessLogRecord>();
    record->event = event;
    record->time = std::chrono::system_clock::now();
    record->session = info_;
    setFields(*record);
    log_->submit(std::move(record), mask);
  }

  std::shared_ptr<AccessLog> log_;
  AccessLogSessionGate gate_;
  std::shared_ptr<AccessLogSessionInfo> info_;
  std::unique_ptr<AccessLogSessionProbe> probe_;
  folly::EventBase* evb_{nullptr};
  std::chrono::steady_clock::time_point openedAt_;
  uint64_t requests_{0};
  bool ended_{false};
};

// The address with its host bits cleared: /24 for IPv4, /48 for IPv6.
std::string truncatePeerAddress(const folly::SocketAddress& addr);

uint64_t msSince(std::chrono::steady_clock::time_point start);
uint64_t
msBetween(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to);

} // namespace openmoq::moqx::logging
