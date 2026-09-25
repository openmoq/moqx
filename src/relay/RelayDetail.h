/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/MoQSession.h>
#include <moxygen/relay/MoQForwarder.h>

#include <folly/Executor.h>
#include <folly/coro/Task.h>

#include <chrono>
#include <memory>
#include <optional>

// Helpers shared by MoqxRelay and its execution-mode subclasses.
namespace openmoq::moqx::detail {

inline constexpr uint8_t kDefaultUpstreamPriority = 128;
inline constexpr std::chrono::seconds kUpstreamConnectWaitTimeout(5);

// Fire-and-forget an upstream-update coroutine on exec — the
// co_withExecutor(getKeepAliveToken(exec), ...).start() idiom shared by the
// forwarder-callback update paths.
void launchUpdate(folly::Executor* exec, folly::coro::Task<void> task);

// Free coroutines, not inline lambdas — captures would dangle past suspension.
folly::coro::Task<void>
doSubscribeUpdate(std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle, bool forward);

folly::coro::Task<void> doNewGroupRequestUpdate(
    std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle,
    uint64_t group
);

// Rejects an AbsoluteRange subscription whose endGroup is already behind the
// forwarder's largest group (the client should FETCH instead). Returns
// std::nullopt when the range is acceptable.
std::optional<moxygen::SubscribeError>
checkRangeNotInPast(moxygen::MoQForwarder& fwd, const moxygen::SubscribeRequest& subReq);

// Derives the upstream SubscribeRequest from a downstream one: fetch from latest at
// upstream priority/default group order, session-assigned requestID, caller's forward.
moxygen::SubscribeRequest makeUpstreamSubReq(
    moxygen::SubscribeRequest base,
    bool forward,
    const std::shared_ptr<moxygen::MoQSession>& upstreamSession
);

folly::coro::Task<void> awaitPublishReply(
    std::shared_ptr<moxygen::MoQForwarder> forwarder, // keeps subscriber's raw ref alive
    std::shared_ptr<moxygen::MoQForwarder::Subscriber> subscriber,
    folly::coro::Task<folly::Expected<moxygen::PublishOk, moxygen::PublishError>> reply
);

// The error returned when addSubscriber yields null (forwarder draining). Shared
// by every subscribe path that attaches to a live forwarder; callers wrap it with
// folly::makeUnexpected for their SubscribeResult.
moxygen::SubscribeError makeAddSubscriberError(moxygen::RequestID requestID);

// Adds a subscriber to fwd, fires any pending NEW_GROUP_REQUEST, and maps a null
// result (forwarder draining) to INTERNAL_ERROR. Shared by the subscribe paths
// that attach to an already-live forwarder.
moxygen::Publisher::SubscribeResult attachSubscriber(
    moxygen::MoQForwarder& fwd,
    std::shared_ptr<moxygen::MoQSession> session,
    const moxygen::SubscribeRequest& subReq,
    std::shared_ptr<moxygen::TrackConsumer> consumer
);

moxygen::TrackStatusOk
buildTrackStatusOk(moxygen::MoQForwarder& fwd, bool hasHandle, const moxygen::TrackStatus& req);

} // namespace openmoq::moqx::detail
