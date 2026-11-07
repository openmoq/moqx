/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/PluginFilters.h"

#include "UpstreamProvider.h" // getPeerRelayID
#include "relay/NullConsumers.h"

#include <folly/io/async/Request.h>
#include <moxygen/MoQFilters.h>

using namespace moxygen;

namespace openmoq::moqx {

using Kind = plugin::Error::Kind;

SessionCloseErrorCode toCloseCode(Kind kind) {
  switch (kind) {
  case Kind::ExpiredToken:
    return SessionCloseErrorCode::EXPIRED_AUTH_TOKEN;
  case Kind::MalformedToken:
    return SessionCloseErrorCode::MALFORMED_AUTH_TOKEN;
  case Kind::Internal:
    return SessionCloseErrorCode::INTERNAL_ERROR;
  case Kind::Unauthorized:
  case Kind::Forbidden:
  case Kind::TooManyTokens:
  case Kind::DoesNotExist:
    return SessionCloseErrorCode::UNAUTHORIZED;
  }
  return SessionCloseErrorCode::INTERNAL_ERROR;
}

std::vector<SetupParameter> toSetupParameters(const plugin::Params& params) {
  std::vector<SetupParameter> out;
  out.reserve(params.size());
  for (const auto& param : params) {
    out.push_back(
        std::visit([&](const auto& value) { return SetupParameter(param.key, value); }, param.value)
    );
  }
  return out;
}

RequestErrorCode toRequestCode(Kind kind) {
  switch (kind) {
  case Kind::ExpiredToken:
    return RequestErrorCode::EXPIRED_AUTH_TOKEN;
  case Kind::MalformedToken:
    return RequestErrorCode::MALFORMED_AUTH_TOKEN;
  case Kind::DoesNotExist:
    return RequestErrorCode::DOES_NOT_EXIST;
  case Kind::Internal:
    return RequestErrorCode::INTERNAL_ERROR;
  case Kind::Unauthorized:
  case Kind::Forbidden:
  case Kind::TooManyTokens:
    return RequestErrorCode::UNAUTHORIZED;
  }
  return RequestErrorCode::INTERNAL_ERROR;
}

namespace {

RequestError requestError(RequestID requestID, const plugin::Error& error) {
  return RequestError{requestID, toRequestCode(error.kind), error.reason};
}

// The args borrow `owner`'s namespace, track and params; the check keeps
// `owner` alive.
template <class Request>
plugin::RequestArgs requestArgs(
    plugin::RequestKind kind,
    const std::shared_ptr<Request>& owner,
    const TrackNamespace& ns,
    std::optional<std::string_view> track,
    const TrackRequestParameters& params,
    bool peer
) {
  return plugin::RequestArgs{
      .kind = kind,
      .ns = ns.trackNamespace,
      .track = track,
      .params = &params,
      .peer = peer,
      .keepAlive = owner,
  };
}

// The session may end, or the request be cancelled, while a suspended check
// resumes; neither may reach the relay.
plugin::Result<folly::Unit> stillLive(
    plugin::Result<folly::Unit> result,
    const plugin::PluginSession& session,
    const folly::CancellationToken& token
) {
  if (result.hasValue() && (session.ended() || token.isCancellationRequested())) {
    return plugin::fail(plugin::Error::Kind::Internal, "request cancelled");
  }
  return result;
}

// `session` is owned here: the filter can go with the session's handlers
// while the check is suspended.
folly::coro::Task<plugin::Result<folly::Unit>>
check(std::shared_ptr<plugin::PluginSession> session, plugin::RequestArgs args) {
  auto token = co_await folly::coro::co_current_cancellation_token;
  auto result = co_await session->authorize(std::move(args), token);
  co_return stillLive(std::move(result), *session, token);
}

// Holds PUBLISH objects' consumer until the check passes. moxygen installs it
// only after the reply resolves (consumerReady=false), by which time the
// relay's consumer is in place.
class DeferredTrackConsumer : public TrackConsumerFilter {
public:
  DeferredTrackConsumer() : TrackConsumerFilter(std::make_shared<NullTrackConsumer>()) {}
};

} // namespace

folly::coro::Task<Publisher::SubscribeResult>
PluginPublisherFilter::subscribe(SubscribeRequest sub, std::shared_ptr<TrackConsumer> consumer) {
  auto req = std::make_shared<SubscribeRequest>(std::move(sub));
  auto peer = peeringEnabled_ && getPeerRelayID(req->params).has_value();
  // Copied before suspending: the filter may be gone when the check resumes.
  auto downstream = downstream_;
  auto ok = co_await check(
      session_,
      requestArgs(
          plugin::RequestKind::Subscribe,
          req,
          req->fullTrackName.trackNamespace,
          req->fullTrackName.trackName,
          req->params,
          peer
      )
  );
  if (ok.hasError()) {
    co_return folly::makeUnexpected(requestError(req->requestID, ok.error()));
  }
  co_return co_await downstream->subscribe(std::move(*req), std::move(consumer));
}

folly::coro::Task<Publisher::FetchResult>
PluginPublisherFilter::fetch(Fetch fetch, std::shared_ptr<FetchConsumer> consumer) {
  auto req = std::make_shared<Fetch>(std::move(fetch));
  auto peer = peeringEnabled_ && getPeerRelayID(req->params).has_value();
  // Copied before suspending: the filter may be gone when the check resumes.
  auto downstream = downstream_;
  auto ok = co_await check(
      session_,
      requestArgs(
          plugin::RequestKind::Fetch,
          req,
          req->fullTrackName.trackNamespace,
          req->fullTrackName.trackName,
          req->params,
          peer
      )
  );
  if (ok.hasError()) {
    co_return folly::makeUnexpected(requestError(req->requestID, ok.error()));
  }
  co_return co_await downstream->fetch(std::move(*req), std::move(consumer));
}

folly::coro::Task<Publisher::SubscribeNamespaceResult> PluginPublisherFilter::subscribeNamespace(
    SubscribeNamespace subNs,
    std::shared_ptr<NamespacePublishHandle> handle
) {
  auto req = std::make_shared<SubscribeNamespace>(std::move(subNs));
  auto peer = peeringEnabled_ && getPeerRelayID(*req).has_value();
  // Copied before suspending: the filter may be gone when the check resumes.
  auto downstream = downstream_;
  auto ok = co_await check(
      session_,
      requestArgs(
          plugin::RequestKind::SubscribeNamespace,
          req,
          req->trackNamespacePrefix,
          std::nullopt,
          req->params,
          peer
      )
  );
  if (ok.hasError()) {
    co_return folly::makeUnexpected(requestError(req->requestID, ok.error()));
  }
  co_return co_await downstream->subscribeNamespace(std::move(*req), std::move(handle));
}

folly::coro::Task<Publisher::SubscribeTracksResult> PluginPublisherFilter::subscribeTracks(
    SubscribeTracks subTracks,
    std::shared_ptr<PublishBlockedHandle> publishBlockedHandle
) {
  auto req = std::make_shared<SubscribeTracks>(std::move(subTracks));
  auto peer = peeringEnabled_ && getPeerRelayID(req->params).has_value();
  // Copied before suspending: the filter may be gone when the check resumes.
  auto downstream = downstream_;
  auto ok = co_await check(
      session_,
      requestArgs(
          plugin::RequestKind::SubscribeTracks,
          req,
          req->trackNamespacePrefix,
          std::nullopt,
          req->params,
          peer
      )
  );
  if (ok.hasError()) {
    co_return folly::makeUnexpected(requestError(req->requestID, ok.error()));
  }
  co_return co_await downstream->subscribeTracks(std::move(*req), std::move(publishBlockedHandle));
}

folly::coro::Task<Publisher::TrackStatusResult>
PluginPublisherFilter::trackStatus(const TrackStatus status) {
  auto req = std::make_shared<TrackStatus>(status);
  auto peer = peeringEnabled_ && getPeerRelayID(req->params).has_value();
  // Copied before suspending: the filter may be gone when the check resumes.
  auto downstream = downstream_;
  auto ok = co_await check(
      session_,
      requestArgs(
          plugin::RequestKind::TrackStatus,
          req,
          req->fullTrackName.trackNamespace,
          req->fullTrackName.trackName,
          req->params,
          peer
      )
  );
  if (ok.hasError()) {
    co_return folly::makeUnexpected(requestError(req->requestID, ok.error()));
  }
  co_return co_await downstream->trackStatus(*req);
}

void PluginPublisherFilter::goaway(Goaway g) {
  downstream_->goaway(std::move(g));
}

Subscriber::PublishResult
PluginSubscriberFilter::publish(PublishRequest pub, std::shared_ptr<SubscriptionHandle> handle) {
  auto req = std::make_shared<PublishRequest>(std::move(pub));
  auto peer = peeringEnabled_ && getPeerRelayID(req->params).has_value();
  auto started = session_->startAuthorize(
      requestArgs(
          plugin::RequestKind::Publish,
          req,
          req->fullTrackName.trackNamespace,
          req->fullTrackName.trackName,
          req->params,
          peer
      ),
      folly::CancellationToken()
  );
  if (started.result.isReady()) {
    auto ok = std::move(started.result).value();
    if (ok.hasError()) {
      return folly::makeUnexpected(requestError(req->requestID, ok.error()));
    }
    return downstream_->publish(std::move(*req), std::move(handle));
  }

  auto deferred = std::make_shared<DeferredTrackConsumer>();
  // The relay's PUBLISH path reads the session from the request context.
  auto context = folly::RequestContext::saveContext();
  auto reply = folly::coro::co_invoke(
      [session = session_,
       started = std::move(started),
       req,
       handle = std::move(handle),
       deferred,
       downstream = downstream_,
       context = std::move(context)](
      ) mutable -> folly::coro::Task<folly::Expected<PublishOk, PublishError>> {
        auto token = co_await folly::coro::co_current_cancellation_token;
        auto ok = stillLive(co_await session->awaitAuthorize(std::move(started)), *session, token);
        if (ok.hasError()) {
          co_return folly::makeUnexpected(requestError(req->requestID, ok.error()));
        }
        PublishResult published = [&] {
          folly::RequestContextScopeGuard guard(context);
          return downstream->publish(std::move(*req), std::move(handle));
        }();
        if (published.hasError()) {
          co_return folly::makeUnexpected(std::move(published.error()));
        }
        deferred->setDownstream(published->consumer);
        co_return co_await std::move(published->reply);
      }
  );
  return PublishConsumerAndReplyTask{
      .consumer = deferred,
      .reply = std::move(reply),
      .consumerReady = false,
  };
}

folly::coro::Task<Subscriber::PublishNamespaceResult> PluginSubscriberFilter::publishNamespace(
    PublishNamespace pubNs,
    std::shared_ptr<PublishNamespaceCallback> callback
) {
  auto req = std::make_shared<PublishNamespace>(std::move(pubNs));
  auto peer = peeringEnabled_ && getPeerRelayID(req->params).has_value();
  // Copied before suspending: the filter may be gone when the check resumes.
  auto downstream = downstream_;
  auto ok = co_await check(
      session_,
      requestArgs(
          plugin::RequestKind::PublishNamespace,
          req,
          req->trackNamespace,
          std::nullopt,
          req->params,
          peer
      )
  );
  if (ok.hasError()) {
    co_return folly::makeUnexpected(requestError(req->requestID, ok.error()));
  }
  co_return co_await downstream->publishNamespace(std::move(*req), std::move(callback));
}

void PluginSubscriberFilter::goaway(Goaway g) {
  downstream_->goaway(std::move(g));
}

} // namespace openmoq::moqx
