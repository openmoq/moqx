/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/RelayDetail.h"

#include <folly/coro/Task.h>
#include <folly/logging/xlog.h>

using namespace moxygen;

namespace openmoq::moqx::detail {

namespace {
bool isV18Plus(const std::shared_ptr<MoQSession>& session) {
  auto version = session ? session->getNegotiatedVersion() : std::optional<uint64_t>{};
  return version.has_value() && getDraftMajorVersion(*version) >= 18;
}
} // namespace

void launchUpdate(folly::Executor* exec, folly::coro::Task<void> task) {
  folly::coro::co_withExecutor(folly::getKeepAliveToken(exec), std::move(task)).start();
}

folly::coro::Task<void>
doSubscribeUpdate(std::shared_ptr<Publisher::SubscriptionHandle> handle, bool forward) {
  auto res = co_await handle->requestUpdate(RequestUpdate{
      RequestID(0),
      handle->subscribeOk().requestID,
      kLocationMin,
      kLocationMax.group,
      kDefaultPriority,
      forward
  });
  if (res.hasError()) {
    XLOG(ERR) << "requestUpdate failed: " << res.error().reasonPhrase;
  }
}

folly::coro::Task<void>
doNewGroupRequestUpdate(std::shared_ptr<Publisher::SubscriptionHandle> handle, uint64_t group) {
  XLOG(DBG4) << "Sending NEW_GROUP_REQUEST update: " << group;
  RequestUpdate update;
  update.requestID = RequestID(0);
  update.existingRequestID = handle->subscribeOk().requestID;
  update.params.insertParam(
      Parameter(folly::to_underlying(TrackRequestParamKey::NEW_GROUP_REQUEST), group)
  );
  auto res = co_await handle->requestUpdate(std::move(update));
  if (res.hasError()) {
    XLOG(ERR) << "NEW_GROUP_REQUEST update failed: " << res.error().reasonPhrase;
  }
}

std::optional<SubscribeError>
checkRangeNotInPast(MoQForwarder& fwd, const SubscribeRequest& subReq) {
  if (fwd.largest() && subReq.locType == LocationType::AbsoluteRange &&
      subReq.endGroup < fwd.largest()->group) {
    return SubscribeError{
        subReq.requestID,
        SubscribeErrorCode::INVALID_RANGE,
        "Range in the past, use FETCH"
    };
  }
  return std::nullopt;
}

SubscribeRequest makeUpstreamSubReq(
    SubscribeRequest base,
    bool forward,
    const std::shared_ptr<MoQSession>& upstreamSession
) {
  // Below v18 key 0x04 is MAX_CACHE_DURATION and forwardable; a v18 upstream would
  // misread it as RENDEZVOUS_TIMEOUT.
  if (isV18Plus(upstreamSession)) {
    base.params.eraseAllParamsOfType(TrackRequestParamKey::RENDEZVOUS_TIMEOUT);
  }
  base.priority = kDefaultUpstreamPriority;
  base.groupOrder = GroupOrder::Default;
  base.locType = LocationType::LargestObject;
  base.forward = forward;
  base.requestID = RequestID(0);
  return base;
}

folly::coro::Task<void> awaitPublishReply(
    std::shared_ptr<MoQForwarder> forwarder,
    std::shared_ptr<MoQForwarder::Subscriber> subscriber,
    folly::coro::Task<folly::Expected<PublishOk, PublishError>> reply
) {
  auto result = co_await co_awaitTry(std::move(reply));
  if (result.hasException()) {
    XLOG(ERR) << "Publish reply exception for " << forwarder->fullTrackName()
              << " subscriber=" << subscriber.get() << ": " << result.exception().what();
    subscriber->unsubscribe();
    co_return;
  }
  if (result.value().hasError()) {
    XLOG(ERR) << "Publish reply error for " << forwarder->fullTrackName()
              << " subscriber=" << subscriber.get() << ": " << result.value().error().reasonPhrase;
    subscriber->unsubscribe();
    co_return;
  }
  XLOG(DBG1) << "Received PublishOk for " << forwarder->fullTrackName()
             << " subscriber=" << subscriber.get();
  subscriber->onPublishOk(result.value().value());
}

SubscribeError makeAddSubscriberError(RequestID requestID) {
  return SubscribeError{requestID, SubscribeErrorCode::INTERNAL_ERROR, "failed to add subscriber"};
}

Publisher::SubscribeResult attachSubscriber(
    MoQForwarder& fwd,
    std::shared_ptr<MoQSession> session,
    const SubscribeRequest& subReq,
    std::shared_ptr<TrackConsumer> consumer
) {
  auto subscriber = fwd.addSubscriber(std::move(session), subReq, std::move(consumer));
  if (!subscriber) {
    XLOG(ERR) << "addSubscriber returned null (draining?) for " << fwd.fullTrackName()
              << " reqID=" << subReq.requestID;
    return folly::makeUnexpected(makeAddSubscriberError(subReq.requestID));
  }
  XLOG(DBG4) << "added subscriber for ftn=" << fwd.fullTrackName();
  fwd.tryProcessNewGroupRequest(subReq.params);
  return subscriber;
}

TrackStatusOk buildTrackStatusOk(MoQForwarder& fwd, bool hasHandle, const TrackStatus& req) {
  TrackStatusCode statusCode = TrackStatusCode::TRACK_NOT_STARTED;
  // largest() set means an object arrived; hasHandle means the upstream sub is still live.
  if (fwd.largest()) {
    statusCode = hasHandle ? TrackStatusCode::IN_PROGRESS : TrackStatusCode::UNKNOWN;
  }
  TrackStatusOk ok;
  ok.requestID = req.requestID;
  ok.groupOrder = fwd.groupOrder();
  ok.largest = fwd.largest();
  ok.fullTrackName = req.fullTrackName;
  ok.statusCode = statusCode;
  return ok;
}

} // namespace openmoq::moqx::detail
