/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "logging/access/AccessLogRequest.h"

#include <array>

#include <folly/Utility.h>
#include <folly/logging/xlog.h>

namespace openmoq::moqx::logging {

namespace {

bool subscriberAsks(AccessLogEvent startEvent) {
  switch (startEvent) {
  case AccessLogEvent::SUBSCRIBE:
  case AccessLogEvent::FETCH:
  case AccessLogEvent::TRACK_STATUS:
  case AccessLogEvent::SUBSCRIBE_NAMESPACE:
  case AccessLogEvent::SUBSCRIBE_TRACKS:
    return true;
  case AccessLogEvent::PUBLISH:
  case AccessLogEvent::PUBLISH_NAMESPACE:
    return false;
  default:
    XDCHECK(false) << "not a request: " << accessLogEventName(startEvent);
    return true;
  }
}

// Names the end line for the subscriber's side or the publisher's side.
AccessLogEvent endEvent(AccessLogEvent startEvent, bool subscriberSide) {
  switch (startEvent) {
  case AccessLogEvent::SUBSCRIBE:
  case AccessLogEvent::PUBLISH:
    return subscriberSide ? AccessLogEvent::UNSUBSCRIBE : AccessLogEvent::PUBLISH_DONE;
  case AccessLogEvent::PUBLISH_NAMESPACE:
    return subscriberSide ? AccessLogEvent::PUBLISH_NAMESPACE_CANCEL
                          : AccessLogEvent::PUBLISH_NAMESPACE_DONE;
  case AccessLogEvent::SUBSCRIBE_NAMESPACE:
    return AccessLogEvent::UNSUBSCRIBE_NAMESPACE;
  case AccessLogEvent::SUBSCRIBE_TRACKS:
    return AccessLogEvent::UNSUBSCRIBE_TRACKS;
  case AccessLogEvent::FETCH:
    return AccessLogEvent::FETCH;
  default:
    XDCHECK(false) << accessLogEventName(startEvent) << " does not have an end line";
    return startEvent;
  }
}

std::string_view publishDoneName(uint64_t code) {
  using Code = moxygen::PublishDoneStatusCode;
  // Indexed by code. 0x7 is not assigned.
  constexpr std::array<std::string_view, 10> kNames = {
      "INTERNAL_ERROR",
      "UNAUTHORIZED",
      "TRACK_ENDED",
      "SUBSCRIPTION_ENDED",
      "GOING_AWAY",
      "TOO_FAR_BEHIND",
      "EXPIRED",
      "?",
      "UPDATE_FAILED",
      "EXCESSIVE_LOAD",
  };
  if (code < kNames.size()) {
    return kNames[code];
  }
  if (code == folly::to_underlying(Code::MALFORMED_TRACK)) {
    return "MALFORMED_TRACK";
  }
  if (code == folly::to_underlying(Code::SESSION_CLOSED)) {
    return "SESSION_CLOSED";
  }
  return "?";
}

} // namespace

std::string_view accessLogEndReasonName(AccessLogEndReason reason, std::optional<uint64_t> code) {
  if (reason == AccessLogEndReason::PublishDone) {
    return code ? publishDoneName(*code) : "?";
  }
  // Indexed by AccessLogEndReason.
  constexpr std::array<std::string_view, 9> kNames = {
      "fin",
      "reset",
      "cancelled",
      "unsubscribed",
      "done",
      "request_error",
      "session_closed",
      "shutdown",
      "?",
  };
  static_assert(kNames.size() == folly::to_underlying(AccessLogEndReason::PublishDone));
  return kNames[folly::to_underlying(reason)];
}

AccessLogStatus AccessLogRequest::fetchStatus(const End& end) {
  return end.isError ? AccessLogStatus::Error : AccessLogStatus::Ok;
}

AccessLogRequest::AccessLogRequest(
    std::shared_ptr<AccessLogSessionContext> context,
    AccessLogEvent startEvent,
    AccessLogDir dir,
    AccessLogRecord logRecord
)
    : context_(std::move(context)), startEvent_(startEvent), logRecord_(std::move(logRecord)),
      start_(std::chrono::steady_clock::now()) {
  XDCHECK(accessLogClass(startEvent_) == AccessLogClass::Request);
  logRecord_.dir = dir;
  context_->countRequest();
}

void AccessLogRequest::setStats(std::shared_ptr<const moxygen::MoQTrackStats> stats, bool sending) {
  // SUB_STATS deltas start from the stats that the request had when they began.
  XDCHECK(!interval_) << "stats set after SUB_STATS started";
  if (stats) {
    stats_ = std::move(stats);
    sending_ = sending;
  }
}

AccessLogRequest::~AccessLogRequest() {
  if (state_ == State::Pending) {
    aborted();
  } else {
    releasedBySession();
  }
}

void AccessLogRequest::ok(std::optional<uint64_t> requestID) {
  if (state_ != State::Pending) {
    return;
  }
  if (requestID) {
    logRecord_.requestID = requestID;
  }
  latencyMs_ = msSince(start_);
  established_ = std::chrono::steady_clock::now();
  switch (startEvent_) {
  case AccessLogEvent::FETCH:
    state_ = State::Established;
    if (end_) {
      writeFetch(fetchStatus(*end_));
    }
    break;
  case AccessLogEvent::TRACK_STATUS:
    writeTransaction(AccessLogStatus::Ok, nullptr);
    state_ = State::Done;
    return;
  default:
    writeTransaction(AccessLogStatus::Ok, nullptr);
    state_ = State::Established;
    if (end_) {
      writeEnd(std::move(*std::exchange(end_, std::nullopt)));
    } else {
      startPeriodic();
    }
    break;
  }
  if (releasedEarly_) {
    releasedBySession();
  }
}

void AccessLogRequest::error(std::optional<uint64_t> requestID, const moxygen::RequestError& err) {
  if (state_ != State::Pending) {
    return;
  }
  if (requestID) {
    logRecord_.requestID = requestID;
  }
  latencyMs_ = msSince(start_);
  state_ = State::Failed;
  if (startEvent_ == AccessLogEvent::FETCH) {
    end_ =
        End{AccessLogEvent::FETCH,
            AccessLogEndReason::RequestError,
            true,
            folly::to_underlying(err.errorCode),
            err.reasonPhrase};
    writeFetch(AccessLogStatus::Error);
    return;
  }
  writeTransaction(AccessLogStatus::Error, &err);
}

void AccessLogRequest::aborted() {
  if (state_ != State::Pending) {
    return;
  }
  latencyMs_ = msSince(start_);
  state_ = State::Done;
  if (startEvent_ == AccessLogEvent::FETCH) {
    end_ = sessionEnd();
    writeFetch(AccessLogStatus::Aborted);
    return;
  }
  writeTransaction(AccessLogStatus::Aborted, nullptr);
}

void AccessLogRequest::ended(
    AccessLogEvent event,
    AccessLogEndReason endReason,
    bool isError,
    std::optional<uint64_t> code,
    std::string reason
) {
  // Teardown calls the same methods as a real end, so a closing session's end comes from
  // sessionEnd().
  auto end = context_->state() == AccessLogSessionState::Open
                 ? End{event, endReason, isError, code, std::move(reason)}
                 : sessionEnd();
  switch (state_) {
  case State::Pending:
    if (!end_) {
      end_ = std::move(end);
    }
    return;
  case State::Established:
    if (startEvent_ == AccessLogEvent::FETCH) {
      end_ = std::move(end);
      writeFetch(fetchStatus(*end_));
    } else {
      writeEnd(std::move(end));
    }
    return;
  case State::Failed:
  case State::Done:
    return;
  }
}

void AccessLogRequest::releasedBySession() {
  switch (state_) {
  case State::Pending:
    releasedEarly_ = true;
    return;
  case State::Established: {
    auto end = sessionEnd();
    ended(end.event, end.endReason, end.isError);
    return;
  }
  case State::Failed:
  case State::Done:
    return;
  }
}

AccessLogRequest::End AccessLogRequest::sessionEnd() const {
  auto peerIsSubscriber = subscriberAsks(startEvent_) == (dir() == AccessLogDir::Rx);
  switch (context_->state()) {
  case AccessLogSessionState::Closed:
    return End{endEvent(startEvent_, peerIsSubscriber), AccessLogEndReason::SessionClosed, false};
  case AccessLogSessionState::ShuttingDown:
    return End{endEvent(startEvent_, !peerIsSubscriber), AccessLogEndReason::Shutdown, false};
  case AccessLogSessionState::Open:
    break;
  }
  // The session ended the request itself, for example with PUBLISH_DONE TOO_FAR_BEHIND.
  return End{endEvent(startEvent_, !peerIsSubscriber), AccessLogEndReason::Unknown, true};
}

void AccessLogRequest::writeTransaction(AccessLogStatus status, const moxygen::RequestError* err) {
  context_->write(startEvent_, status != AccessLogStatus::Ok, logRecord_, [&](auto& record) {
    record.status = status;
    record.latencyMs = latencyMs_;
    if (err) {
      record.errorCode = folly::to_underlying(err->errorCode);
      record.reason = err->reasonPhrase;
    }
  });
}

void AccessLogRequest::setEnd(AccessLogRecord& record, End& end) {
  record.endReason = std::string(accessLogEndReasonName(end.endReason, end.code));
  record.errorCode = end.code;
  record.reason = std::move(end.reason);
}

void AccessLogRequest::writeEnd(End end) {
  state_ = State::Done;
  context_->write(end.event, end.isError, logRecord_, [&](auto& record) {
    setEnd(record, end);
    setTotals(record, established_);
    record.ageMs = msSince(established_);
    if (startEvent_ == AccessLogEvent::SUBSCRIBE_NAMESPACE ||
        startEvent_ == AccessLogEvent::SUBSCRIBE_TRACKS) {
      record.namespaceMsgs = namespaceMsgs_;
      record.namespaceDoneMsgs = namespaceDoneMsgs_;
      record.publishBlockedMsgs = publishBlockedMsgs_;
    }
  });
}

void AccessLogRequest::setTotals(
    AccessLogRecord& record,
    std::chrono::steady_clock::time_point from
) const {
  if (!stats_) {
    return;
  }
  record.objects = stats_->objects;
  record.bytes = stats_->bytes;
  record.groups = stats_->groups;
  record.subgroups = stats_->subgroups;
  record.datagrams = stats_->datagrams;
  record.statusObjects = stats_->statusObjects;
  record.resets = stats_->resets;
  record.lastResetCode = stats_->lastResetCode;
  record.openSubgroups = stats_->openSubgroups;
  if (sending_) {
    record.bytesQueued = stats_->bytesQueued;
    record.payloadBytesQueued = stats_->payloadBytesQueued;
    record.bytesWritten = stats_->bytesWritten;
    record.bytesAcked = stats_->bytesAcked;
    record.bytesDropped = stats_->bytesDropped;
    record.bytesCancelled = stats_->bytesCancelled;
    record.objectsWritten = stats_->objectsWritten;
    record.objectsAcked = stats_->objectsAcked;
    if (stats_->objectsAcked > 0) {
      record.ackUsAvg = stats_->ackLatencyUsSum / stats_->objectsAcked;
      record.ackUsMax = stats_->ackLatencyUsMax;
    }
  }
  // ttfo_ms counts from the reply for a subscription and from the request for a FETCH. A
  // subscription's data can arrive before its reply.
  if (stats_->firstObject) {
    record.ttfoMs = *stats_->firstObject > from ? msBetween(from, *stats_->firstObject) : 0;
  }
}

void AccessLogRequest::startPeriodic() {
  if (!stats_ || !logRecord_.requestID) {
    return;
  }
  auto mask = context_->periodicMask(*logRecord_.requestID);
  if (mask == 0 || !context_->schedulePeriodic(weak_from_this())) {
    return;
  }
  interval_ =
      Interval{mask, *stats_, std::chrono::system_clock::now(), std::chrono::steady_clock::now()};
}

bool AccessLogRequest::writePeriodic() {
  if (state_ != State::Established || !interval_) {
    return false;
  }
  const auto& last = interval_->stats;
  const auto& now = *stats_;
  context_->writeTo(interval_->mask, AccessLogEvent::SUB_STATS, logRecord_, [&](auto& record) {
    record.intervalStart = interval_->start;
    record.intervalMs = msSince(interval_->steadyStart);
    record.dObjects = now.objects - last.objects;
    record.dBytes = now.bytes - last.bytes;
    record.dGroups = now.groups - last.groups;
    record.dSubgroups = now.subgroups - last.subgroups;
    record.dDatagrams = now.datagrams - last.datagrams;
    record.dStatusObjects = now.statusObjects - last.statusObjects;
    record.dResets = now.resets - last.resets;
    if (sending_) {
      record.dBytesQueued = now.bytesQueued - last.bytesQueued;
      record.dPayloadBytesQueued = now.payloadBytesQueued - last.payloadBytesQueued;
      record.dBytesWritten = now.bytesWritten - last.bytesWritten;
      record.dBytesAcked = now.bytesAcked - last.bytesAcked;
      record.dObjectsWritten = now.objectsWritten - last.objectsWritten;
      record.dObjectsAcked = now.objectsAcked - last.objectsAcked;
      if (auto acked = now.objectsAcked - last.objectsAcked; acked > 0) {
        record.dAckUsAvg = (now.ackLatencyUsSum - last.ackLatencyUsSum) / acked;
      }
    }
    setTotals(record, established_);
    record.ageMs = msSince(established_);
    context_->addTransportStats(record);
  });
  interval_->stats = now;
  interval_->start = std::chrono::system_clock::now();
  interval_->steadyStart = std::chrono::steady_clock::now();
  return true;
}

void AccessLogRequest::writeFetch(AccessLogStatus status) {
  state_ = State::Done;
  auto end = std::move(*std::exchange(end_, std::nullopt));
  auto isError = status != AccessLogStatus::Ok;
  context_->write(AccessLogEvent::FETCH, isError, logRecord_, [&](auto& record) {
    setEnd(record, end);
    setTotals(record, start_);
    record.status = status;
    record.latencyMs = latencyMs_;
    record.fetchMs = msSince(start_);
  });
}

void AccessLogRequest::writeMessage(
    AccessLogEvent event,
    const moxygen::TrackNamespace& suffix,
    std::string_view trackName
) {
  switch (event) {
  case AccessLogEvent::NAMESPACE:
    namespaceMsgs_++;
    break;
  case AccessLogEvent::NAMESPACE_DONE:
    namespaceDoneMsgs_++;
    break;
  case AccessLogEvent::PUBLISH_BLOCKED:
    publishBlockedMsgs_++;
    break;
  default:
    break;
  }
  context_->write(event, false, logRecord_, [&](auto& record) {
    auto ftn = std::make_shared<moxygen::FullTrackName>();
    if (logRecord_.ftn) {
      ftn->trackNamespace = logRecord_.ftn->trackNamespace;
    }
    for (const auto& element : suffix.trackNamespace) {
      ftn->trackNamespace.trackNamespace.push_back(element);
    }
    ftn->trackName = std::string(trackName);
    record.ftn = std::move(ftn);
    record.namespaceOnly = event != AccessLogEvent::PUBLISH_BLOCKED;
  });
}

void AccessLogRequest::writeRequestUpdate(
    AccessLogDir dir,
    std::optional<uint64_t> requestID,
    std::optional<uint8_t> priority,
    std::optional<bool> forward,
    std::chrono::steady_clock::time_point start,
    const folly::Expected<moxygen::RequestOk, moxygen::RequestError>& result
) {
  context_->write(AccessLogEvent::REQUEST_UPDATE, result.hasError(), logRecord_, [&](auto& record) {
    record.dir = dir;
    record.requestID = requestID;
    record.priority = priority;
    record.forward = forward;
    record.latencyMs = msSince(start);
    if (result.hasValue()) {
      record.status = AccessLogStatus::Ok;
    } else {
      record.status = AccessLogStatus::Error;
      record.errorCode = folly::to_underlying(result.error().errorCode);
      record.reason = result.error().reasonPhrase;
    }
  });
}

} // namespace openmoq::moqx::logging
