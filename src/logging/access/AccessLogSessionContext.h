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
#include <moxygen/MoQSession.h>

#include "logging/access/AccessLog.h"

namespace openmoq::moqx::logging {

enum class AccessLogSessionState : uint8_t { Open, Closed, ShuttingDown };

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
  void setup(
      const std::shared_ptr<moxygen::MoQSession>& session,
      std::string service,
      std::string peerClass,
      std::string upstream = {},
      const folly::SocketAddress& local = {}
  );

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

  void writeSetup(
      AccessLogStatus status,
      std::optional<uint64_t> errorCode = std::nullopt,
      std::string reason = {}
  );
  // Writes SESSION_END once.
  void writeSessionEnd(const moxygen::MoQSession& session);
  void writeSessionEnd(std::optional<uint64_t> errorCode);

  // ShuttingDown during relay shutdown, and Closed once the session is closing or
  // destroyed. close() sets the closed flag before it tears down requests, so after that
  // every end is teardown. Open before setup() and while the session is open.
  AccessLogSessionState state() const;

  // False when the session's service selects zero sinks for its peer class, so its requests
  // are not tracked.
  bool active() const { return gate_.matched != 0; }
  // True once setup() has found that the session reaches zero sinks.
  bool filteredOut() const { return exec_ && !active(); }

  void countRequest() { ++requests_; }

private:
  template <typename SetFields>
  void
  writeFrom(AccessLogEvent event, bool isError, const AccessLogRecord* base, SetFields& setFields) {
    auto mask = log_->eventMask(gate_, event, isError);
    if (mask == 0) {
      return;
    }
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
  std::weak_ptr<moxygen::MoQSession> session_;
  folly::Executor* exec_{nullptr};
  std::chrono::steady_clock::time_point openedAt_;
  uint64_t requests_{0};
  bool ended_{false};
};

// The address with its host bits cleared: /24 for IPv4, /48 for IPv6.
std::string truncatePeerAddress(const folly::SocketAddress& addr);

uint64_t msSince(std::chrono::steady_clock::time_point start);

} // namespace openmoq::moqx::logging
