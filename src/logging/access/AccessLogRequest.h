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

#include <moxygen/MoQTypes.h>

#include "logging/access/AccessLogSessionContext.h"

namespace openmoq::moqx::logging {

enum class AccessLogEndReason : uint8_t {
  Fin,
  Reset,
  Cancelled,
  Unsubscribed,
  Done,
  RequestError,
  SessionClosed,
  Shutdown,
  Unknown,
  // The PUBLISH_DONE status code names it.
  PublishDone,
};

// The end_reason text. code is the PUBLISH_DONE status code for PublishDone.
std::string_view accessLogEndReasonName(AccessLogEndReason reason, std::optional<uint64_t> code);

// One request's log lines: a transaction line at the reply, then one end line for a
// long-lived request. FETCH has a single line, written once the reply and the end of its data
// are both known. Its wrappers share it, and every call runs on the session's executor.
class AccessLogRequest {
public:
  // startEvent names the request: SUBSCRIBE, FETCH, PUBLISH_NAMESPACE and so on. Each line
  // starts as a copy of logRecord, which holds the request's identity and parameters.
  AccessLogRequest(
      std::shared_ptr<AccessLogSessionContext> context,
      AccessLogEvent startEvent,
      AccessLogDir dir,
      AccessLogRecord logRecord
  );
  // Writes the lines still pending, as aborted or ended by the session.
  ~AccessLogRequest();

  AccessLogRequest(const AccessLogRequest&) = delete;
  AccessLogRequest& operator=(const AccessLogRequest&) = delete;

  // requestID, when set, replaces the ID in logRecord. It is unset when the request did not get
  // an ID.
  void ok(std::optional<uint64_t> requestID);
  void error(std::optional<uint64_t> requestID, const moxygen::RequestError& err);
  void aborted();

  // An established request ended. Only the first end is written.
  void ended(
      AccessLogEvent event,
      AccessLogEndReason endReason,
      bool isError,
      std::optional<uint64_t> code = std::nullopt,
      std::string reason = {}
  );
  // The session released the request without the relay ending it. The session closed, or
  // it ended the request itself. Before the reply, the end line waits for it.
  void releasedBySession();

  // A NAMESPACE, NAMESPACE_DONE or PUBLISH_BLOCKED within this request. suffix extends
  // the request's namespace prefix.
  void writeMessage(
      AccessLogEvent event,
      const moxygen::TrackNamespace& suffix,
      std::string_view trackName = {}
  );

  // dir is the update's own direction: the subscriber sends a PUBLISH's updates.
  void writeRequestUpdate(
      AccessLogDir dir,
      std::optional<uint64_t> requestID,
      std::optional<uint8_t> priority,
      std::optional<bool> forward,
      std::chrono::steady_clock::time_point start,
      const folly::Expected<moxygen::RequestOk, moxygen::RequestError>& result
  );

  AccessLogDir dir() const { return logRecord_.dir.value_or(AccessLogDir::Rx); }

private:
  struct End {
    AccessLogEvent event;
    AccessLogEndReason endReason;
    bool isError{false};
    std::optional<uint64_t> code;
    std::string reason;
  };

  enum class State : uint8_t { Pending, Established, Failed, Done };

  // The request and its reply, written when the reply arrives.
  void writeTransaction(AccessLogStatus status, const moxygen::RequestError* err);
  static void setEnd(AccessLogRecord& record, End& end);
  void writeEnd(End end);
  void writeFetch(AccessLogStatus status);
  static AccessLogStatus fetchStatus(const End& end);
  // The end line for a request that the session released. It is named for the peer's side
  // when the session went away, and for the relay's side otherwise.
  End sessionEnd() const;

  std::shared_ptr<AccessLogSessionContext> context_;
  AccessLogEvent startEvent_;
  AccessLogRecord logRecord_;
  std::chrono::steady_clock::time_point start_;
  std::chrono::steady_clock::time_point established_;
  State state_{State::Pending};
  std::optional<uint64_t> latencyMs_;
  // An end that arrived before the reply, or a FETCH's end of data.
  std::optional<End> end_;
  // The session released the request before its reply. A FETCH's data can end first.
  bool releasedEarly_{false};
};

} // namespace openmoq::moqx::logging
