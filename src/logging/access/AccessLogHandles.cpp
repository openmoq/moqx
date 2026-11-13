/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "logging/access/AccessLogHandles.h"

#include <folly/coro/Task.h>
#include <moxygen/MoQFilters.h>
#include <moxygen/MoQSession.h>

namespace openmoq::moqx::logging {

namespace {

using moxygen::MoQPublishError;
using moxygen::RequestError;
using moxygen::RequestOk;
using moxygen::RequestUpdate;
using RequestUpdateResult = folly::Expected<RequestOk, RequestError>;

bool publishDoneIsError(moxygen::PublishDoneStatusCode code) {
  using Code = moxygen::PublishDoneStatusCode;
  return code != Code::TRACK_ENDED && code != Code::SUBSCRIPTION_ENDED &&
         code != Code::GOING_AWAY && code != Code::EXPIRED;
}

// Tells the request when the session releases the wrapper that it holds.
class RequestRef {
public:
  RequestRef(std::shared_ptr<AccessLogRequest> logRequest, bool sessionHeld)
      : logRequest_(std::move(logRequest)), sessionHeld_(sessionHeld) {}
  ~RequestRef() {
    if (sessionHeld_) {
      logRequest_->releasedBySession();
    }
  }
  RequestRef(const RequestRef&) = delete;
  RequestRef& operator=(const RequestRef&) = delete;

  AccessLogRequest& operator*() const { return *logRequest_; }
  AccessLogRequest* operator->() const { return logRequest_.get(); }
  const std::shared_ptr<AccessLogRequest>& get() const { return logRequest_; }
  // The session calls the wrapper that it holds for an update from the peer.
  AccessLogDir updateDir() const { return sessionHeld_ ? AccessLogDir::Rx : AccessLogDir::Tx; }

private:
  std::shared_ptr<AccessLogRequest> logRequest_;
  bool sessionHeld_;
};

// An rx REQUEST_UPDATE carries its ID. The session assigns a tx one's ID, so it comes from
// the reply.
template <typename Inner>
folly::coro::Task<RequestUpdateResult> loggedUpdate(
    std::shared_ptr<Inner> inner,
    std::shared_ptr<AccessLogRequest> logRequest,
    AccessLogDir dir,
    RequestUpdate update
) {
  auto start = std::chrono::steady_clock::now();
  std::optional<uint64_t> requestID;
  if (dir == AccessLogDir::Rx) {
    requestID = update.requestID.value;
  }
  auto priority = update.priority;
  auto forward = update.forward;
  auto result = co_await folly::coro::co_awaitTry(inner->requestUpdate(std::move(update)));
  if (result.hasException()) {
    logRequest->writeRequestUpdate(
        dir,
        requestID,
        priority,
        forward,
        start,
        folly::makeUnexpected(RequestError{
            0,
            moxygen::RequestErrorCode::INTERNAL_ERROR,
            std::string(result.exception().what())
        })
    );
    co_yield folly::coro::co_error(std::move(result.exception()));
  }
  if (dir == AccessLogDir::Tx) {
    auto id = result->hasValue() ? (*result)->requestID : result->error().requestID;
    if (id != moxygen::MoQSession::failedLocalRequestID()) {
      requestID = id.value;
    }
  }
  logRequest->writeRequestUpdate(dir, requestID, priority, forward, start, *result);
  co_return std::move(*result);
}

// Holds the inner object and the request for one wrapper.
template <typename Interface> class LoggedHandle : public Interface {
public:
  LoggedHandle(
      std::shared_ptr<Interface> inner,
      std::shared_ptr<AccessLogRequest> logRequest,
      bool sessionHeld
  )
      : inner_(std::move(inner)), logRequest_(std::move(logRequest), sessionHeld) {}

protected:
  std::shared_ptr<Interface> inner_;
  RequestRef logRequest_;
};

// A handle that carries REQUEST_UPDATE.
template <typename Interface> class LoggedUpdatable : public LoggedHandle<Interface> {
public:
  using LoggedHandle<Interface>::LoggedHandle;

  folly::coro::Task<RequestUpdateResult> requestUpdate(RequestUpdate update) override {
    return loggedUpdate(
        this->inner_,
        this->logRequest_.get(),
        this->logRequest_.updateDir(),
        std::move(update)
    );
  }
};

class LoggedSubscriptionHandle : public LoggedUpdatable<moxygen::SubscriptionHandle> {
public:
  LoggedSubscriptionHandle(
      std::shared_ptr<moxygen::SubscriptionHandle> inner,
      std::shared_ptr<AccessLogRequest> logRequest,
      bool sessionHeld
  )
      : LoggedUpdatable(std::move(inner), std::move(logRequest), sessionHeld) {
    if (inner_->hasSubscribeOk()) {
      setSubscribeOk(inner_->subscribeOk());
    }
  }

  const moxygen::SubscribeOk& subscribeOk() const override { return inner_->subscribeOk(); }

  void unsubscribe() override {
    logRequest_->ended(AccessLogEvent::UNSUBSCRIBE, AccessLogEndReason::Unsubscribed, false);
    inner_->unsubscribe();
  }

  void cancelFill(moxygen::RequestID requestID) override { inner_->cancelFill(requestID); }
};

class LoggedTrackConsumer : public moxygen::TrackConsumerFilter {
public:
  LoggedTrackConsumer(
      std::shared_ptr<moxygen::TrackConsumer> inner,
      std::shared_ptr<AccessLogRequest> logRequest,
      bool sessionHeld
  )
      : TrackConsumerFilter(std::move(inner)), logRequest_(std::move(logRequest), sessionHeld) {}

  folly::Expected<std::shared_ptr<moxygen::FetchConsumer>, MoQPublishError>
  beginFill(moxygen::RequestID requestID) override {
    return downstream_->beginFill(requestID);
  }

  void goaway(moxygen::Goaway goaway) override { downstream_->goaway(std::move(goaway)); }

  folly::Expected<folly::Unit, MoQPublishError> publishDone(moxygen::PublishDone pubDone) override {
    logRequest_->ended(
        AccessLogEvent::PUBLISH_DONE,
        AccessLogEndReason::PublishDone,
        publishDoneIsError(pubDone.statusCode),
        folly::to_underlying(pubDone.statusCode),
        pubDone.reasonPhrase
    );
    return TrackConsumerFilter::publishDone(std::move(pubDone));
  }

private:
  RequestRef logRequest_;
};

class LoggedFetchHandle : public LoggedUpdatable<moxygen::Publisher::FetchHandle> {
public:
  using LoggedUpdatable::LoggedUpdatable;

  const moxygen::FetchOk& fetchOk() const override { return inner_->fetchOk(); }

  void fetchCancel() override {
    logRequest_->ended(AccessLogEvent::FETCH, AccessLogEndReason::Cancelled, false);
    inner_->fetchCancel();
  }
};

class LoggedFetchConsumer : public LoggedHandle<moxygen::FetchConsumer> {
public:
  using LoggedHandle::LoggedHandle;

  folly::Expected<folly::Unit, MoQPublishError> object(
      uint64_t groupID,
      uint64_t subgroupID,
      uint64_t objectID,
      moxygen::Payload payload,
      moxygen::Extensions extensions,
      bool finFetch,
      bool forwardingPreferenceIsDatagram
  ) override {
    finIf(finFetch);
    return inner_->object(
        groupID,
        subgroupID,
        objectID,
        std::move(payload),
        std::move(extensions),
        finFetch,
        forwardingPreferenceIsDatagram
    );
  }

  void checkpoint() override { inner_->checkpoint(); }

  folly::Expected<folly::Unit, MoQPublishError> beginObject(
      uint64_t groupID,
      uint64_t subgroupID,
      uint64_t objectID,
      uint64_t length,
      moxygen::Payload initialPayload,
      moxygen::Extensions extensions
  ) override {
    return inner_->beginObject(
        groupID,
        subgroupID,
        objectID,
        length,
        std::move(initialPayload),
        std::move(extensions)
    );
  }

  folly::Expected<moxygen::ObjectPublishStatus, MoQPublishError>
  objectPayload(moxygen::Payload payload, bool finFetch) override {
    finIf(finFetch);
    return inner_->objectPayload(std::move(payload), finFetch);
  }

  folly::Expected<folly::Unit, MoQPublishError>
  endOfGroup(uint64_t groupID, uint64_t subgroupID, uint64_t objectID, bool finFetch) override {
    finIf(finFetch);
    return inner_->endOfGroup(groupID, subgroupID, objectID, finFetch);
  }

  folly::Expected<folly::Unit, MoQPublishError>
  endOfTrackAndGroup(uint64_t groupID, uint64_t subgroupID, uint64_t objectID) override {
    finIf(true);
    return inner_->endOfTrackAndGroup(groupID, subgroupID, objectID);
  }

  folly::Expected<folly::Unit, MoQPublishError> endOfFetch() override {
    finIf(true);
    return inner_->endOfFetch();
  }

  void reset(moxygen::ResetStreamErrorCode error) override {
    logRequest_->ended(
        AccessLogEvent::FETCH,
        AccessLogEndReason::Reset,
        true,
        folly::to_underlying(error)
    );
    inner_->reset(error);
  }

  void goaway(moxygen::Goaway goaway) override { inner_->goaway(std::move(goaway)); }

  folly::Expected<folly::SemiFuture<uint64_t>, MoQPublishError> awaitReadyToConsume() override {
    return inner_->awaitReadyToConsume();
  }

  folly::Expected<folly::Unit, MoQPublishError>
  endOfUnknownRange(uint64_t groupID, uint64_t objectID, bool finFetch) override {
    finIf(finFetch);
    return inner_->endOfUnknownRange(groupID, objectID, finFetch);
  }

private:
  void finIf(bool fin) {
    if (fin) {
      logRequest_->ended(AccessLogEvent::FETCH, AccessLogEndReason::Fin, false);
    }
  }
};

class LoggedSubscribeNamespaceHandle
    : public LoggedUpdatable<moxygen::Publisher::SubscribeNamespaceHandle> {
public:
  using LoggedUpdatable::LoggedUpdatable;

  const moxygen::SubscribeNamespaceOk& subscribeNamespaceOk() const override {
    return inner_->subscribeNamespaceOk();
  }

  void unsubscribeNamespace() override {
    logRequest_
        ->ended(AccessLogEvent::UNSUBSCRIBE_NAMESPACE, AccessLogEndReason::Unsubscribed, false);
    inner_->unsubscribeNamespace();
  }
};

class LoggedNamespacePublishHandle
    : public LoggedHandle<moxygen::Publisher::NamespacePublishHandle> {
public:
  using LoggedHandle::LoggedHandle;

  void namespaceMsg(const moxygen::Namespace& ns) override {
    logRequest_->writeMessage(AccessLogEvent::NAMESPACE, ns.trackNamespaceSuffix);
    if (inner_) {
      inner_->namespaceMsg(ns);
    }
  }
  void namespaceMsg(const moxygen::TrackNamespace& suffix) override {
    logRequest_->writeMessage(AccessLogEvent::NAMESPACE, suffix);
    if (inner_) {
      inner_->namespaceMsg(suffix);
    }
  }
  void namespaceDoneMsg(const moxygen::TrackNamespace& suffix) override {
    logRequest_->writeMessage(AccessLogEvent::NAMESPACE_DONE, suffix);
    if (inner_) {
      inner_->namespaceDoneMsg(suffix);
    }
  }
};

class LoggedSubscribeTracksHandle
    : public LoggedUpdatable<moxygen::Publisher::SubscribeTracksHandle> {
public:
  using LoggedUpdatable::LoggedUpdatable;

  const moxygen::SubscribeTracksOk& subscribeTracksOk() const override {
    return inner_->subscribeTracksOk();
  }

  void unsubscribeTracks() override {
    logRequest_->ended(AccessLogEvent::UNSUBSCRIBE_TRACKS, AccessLogEndReason::Unsubscribed, false);
    inner_->unsubscribeTracks();
  }
};

class LoggedPublishBlockedHandle : public LoggedHandle<moxygen::Publisher::PublishBlockedHandle> {
public:
  using LoggedHandle::LoggedHandle;

  void
  publishBlocked(const moxygen::TrackNamespace& suffix, const std::string& trackName) override {
    logRequest_->writeMessage(AccessLogEvent::PUBLISH_BLOCKED, suffix, trackName);
    if (inner_) {
      inner_->publishBlocked(suffix, trackName);
    }
  }
};

class LoggedPublishNamespaceHandle
    : public LoggedUpdatable<moxygen::Subscriber::PublishNamespaceHandle> {
public:
  using LoggedUpdatable::LoggedUpdatable;

  const moxygen::PublishNamespaceOk& publishNamespaceOk() const override {
    return inner_->publishNamespaceOk();
  }

  void publishNamespaceDone() override {
    logRequest_->ended(AccessLogEvent::PUBLISH_NAMESPACE_DONE, AccessLogEndReason::Done, false);
    inner_->publishNamespaceDone();
  }
};

class LoggedPublishNamespaceCallback
    : public LoggedHandle<moxygen::Subscriber::PublishNamespaceCallback> {
public:
  using LoggedHandle::LoggedHandle;

  void publishNamespaceCancel(moxygen::PublishNamespaceErrorCode errorCode, std::string reason)
      override {
    logRequest_->ended(
        AccessLogEvent::PUBLISH_NAMESPACE_CANCEL,
        AccessLogEndReason::Cancelled,
        false,
        folly::to_underlying(errorCode),
        reason
    );
    inner_->publishNamespaceCancel(errorCode, std::move(reason));
  }
};

// The wrapper for each interface. The session ignores a null namespace or blocked handle,
// so those get a wrapper even for a null inner.
template <typename Interface> struct LoggedFor;
template <> struct LoggedFor<moxygen::SubscriptionHandle> {
  using type = LoggedSubscriptionHandle;
};
template <> struct LoggedFor<moxygen::TrackConsumer> {
  using type = LoggedTrackConsumer;
};
template <> struct LoggedFor<moxygen::Publisher::FetchHandle> {
  using type = LoggedFetchHandle;
};
template <> struct LoggedFor<moxygen::FetchConsumer> {
  using type = LoggedFetchConsumer;
};
template <> struct LoggedFor<moxygen::Publisher::SubscribeNamespaceHandle> {
  using type = LoggedSubscribeNamespaceHandle;
};
template <> struct LoggedFor<moxygen::Publisher::NamespacePublishHandle> {
  using type = LoggedNamespacePublishHandle;
  static constexpr bool kWrapNull = true;
};
template <> struct LoggedFor<moxygen::Publisher::SubscribeTracksHandle> {
  using type = LoggedSubscribeTracksHandle;
};
template <> struct LoggedFor<moxygen::Publisher::PublishBlockedHandle> {
  using type = LoggedPublishBlockedHandle;
  static constexpr bool kWrapNull = true;
};
template <> struct LoggedFor<moxygen::Subscriber::PublishNamespaceHandle> {
  using type = LoggedPublishNamespaceHandle;
};
template <> struct LoggedFor<moxygen::Subscriber::PublishNamespaceCallback> {
  using type = LoggedPublishNamespaceCallback;
};

template <typename Interface> constexpr bool wrapsNull() {
  if constexpr (requires { LoggedFor<Interface>::kWrapNull; }) {
    return LoggedFor<Interface>::kWrapNull;
  }
  return false;
}

struct AbortUnlessReplied {
  std::shared_ptr<AccessLogRequest> logRequest;
  AbortUnlessReplied(std::shared_ptr<AccessLogRequest> r) : logRequest(std::move(r)) {}
  AbortUnlessReplied(AbortUnlessReplied&&) = default;
  ~AbortUnlessReplied() {
    if (logRequest) {
      logRequest->aborted();
    }
  }
};

folly::coro::Task<folly::Expected<moxygen::PublishOk, moxygen::PublishError>> awaitPublishReply(
    folly::coro::Task<folly::Expected<moxygen::PublishOk, moxygen::PublishError>> reply,
    AbortUnlessReplied guard
) {
  auto& logRequest = *guard.logRequest;
  auto result = co_await folly::coro::co_awaitTry(std::move(reply));
  if (result.hasException()) {
    logRequest.aborted();
    co_yield folly::coro::co_error(std::move(result.exception()));
  }
  // The session assigns a tx PUBLISH's request ID before it sends the PUBLISH.
  if (result->hasValue()) {
    logRequest.ok(
        logRequest.dir() == AccessLogDir::Tx ? std::optional<uint64_t>((*result)->requestID.value)
                                             : std::nullopt
    );
  } else {
    logRequest.error(
        logRequest.dir() == AccessLogDir::Tx ? txRequestID(result->error().requestID)
                                             : std::nullopt,
        result->error()
    );
  }
  co_return std::move(*result);
}

} // namespace

std::optional<uint64_t> txRequestID(moxygen::RequestID id) {
  if (id == moxygen::MoQSession::failedLocalRequestID()) {
    return std::nullopt;
  }
  return id.value;
}

template <typename Interface>
std::shared_ptr<Interface> logged(
    std::shared_ptr<Interface> inner,
    std::shared_ptr<AccessLogRequest> logRequest,
    bool sessionHeld
) {
  if (!inner && !wrapsNull<Interface>()) {
    return nullptr;
  }
  return std::make_shared<typename LoggedFor<Interface>::type>(
      std::move(inner),
      std::move(logRequest),
      sessionHeld
  );
}

#define MOQX_LOGGED(Interface)                                                                     \
  template std::shared_ptr<Interface> logged<Interface>(                                           \
      std::shared_ptr<Interface>,                                                                  \
      std::shared_ptr<AccessLogRequest>,                                                           \
      bool                                                                                         \
  );
MOQX_LOGGED(moxygen::SubscriptionHandle)
MOQX_LOGGED(moxygen::TrackConsumer)
MOQX_LOGGED(moxygen::Publisher::FetchHandle)
MOQX_LOGGED(moxygen::FetchConsumer)
MOQX_LOGGED(moxygen::Publisher::SubscribeNamespaceHandle)
MOQX_LOGGED(moxygen::Publisher::NamespacePublishHandle)
MOQX_LOGGED(moxygen::Publisher::SubscribeTracksHandle)
MOQX_LOGGED(moxygen::Publisher::PublishBlockedHandle)
MOQX_LOGGED(moxygen::Subscriber::PublishNamespaceHandle)
MOQX_LOGGED(moxygen::Subscriber::PublishNamespaceCallback)
#undef MOQX_LOGGED

folly::coro::Task<folly::Expected<moxygen::PublishOk, moxygen::PublishError>> logPublishReply(
    folly::coro::Task<folly::Expected<moxygen::PublishOk, moxygen::PublishError>> reply,
    std::shared_ptr<AccessLogRequest> logRequest
) {
  return awaitPublishReply(std::move(reply), AbortUnlessReplied(std::move(logRequest)));
}

} // namespace openmoq::moqx::logging
