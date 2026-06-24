/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/AuthFilters.h"

#include "UpstreamProvider.h" // getPeerRelayID

using namespace moxygen;

namespace openmoq::moqx {

folly::coro::Task<Publisher::SubscribeResult>
AuthPublisherFilter::subscribe(SubscribeRequest sub, std::shared_ptr<TrackConsumer> consumer) {
  auto ok = auth::authorize(
      *verifier_,
      auth::Action::Subscribe,
      sub.params,
      sub.fullTrackName.trackNamespace,
      *grants_,
      sub.fullTrackName.trackName
  );
  if (ok.hasError()) {
    return folly::coro::makeTask<Publisher::SubscribeResult>(folly::makeUnexpected(
        SubscribeError{sub.requestID, SubscribeErrorCode::UNAUTHORIZED, auth::toString(ok.error())}
    ));
  }
  return downstream_->subscribe(std::move(sub), std::move(consumer));
}

folly::coro::Task<Publisher::FetchResult>
AuthPublisherFilter::fetch(Fetch fetch, std::shared_ptr<FetchConsumer> consumer) {
  auto ok = auth::authorize(
      *verifier_,
      auth::Action::Fetch,
      fetch.params,
      fetch.fullTrackName.trackNamespace,
      *grants_,
      fetch.fullTrackName.trackName
  );
  if (ok.hasError()) {
    return folly::coro::makeTask<Publisher::FetchResult>(folly::makeUnexpected(
        FetchError{fetch.requestID, FetchErrorCode::UNAUTHORIZED, auth::toString(ok.error())}
    ));
  }
  return downstream_->fetch(std::move(fetch), std::move(consumer));
}

folly::coro::Task<Publisher::SubscribeNamespaceResult> AuthPublisherFilter::subscribeNamespace(
    SubscribeNamespace subNs,
    std::shared_ptr<NamespacePublishHandle> handle
) {
  // Peer relays authenticate via their relay token, validated downstream;
  // only non-peer subscribers go through grant-based auth here.
  bool isPeer = peeringEnabled_ && getPeerRelayID(subNs).has_value();
  if (!isPeer) {
    auto ok = auth::authorize(
        *verifier_,
        auth::Action::SubscribeNamespace,
        subNs.params,
        subNs.trackNamespacePrefix,
        *grants_
    );
    if (ok.hasError()) {
      return folly::coro::makeTask<Publisher::SubscribeNamespaceResult>(
          folly::makeUnexpected(SubscribeNamespaceError{
              subNs.requestID,
              SubscribeNamespaceErrorCode::UNAUTHORIZED,
              auth::toString(ok.error())
          })
      );
    }
  }
  return downstream_->subscribeNamespace(std::move(subNs), std::move(handle));
}

folly::coro::Task<Publisher::SubscribeTracksResult> AuthPublisherFilter::subscribeTracks(
    SubscribeTracks subTracks,
    std::shared_ptr<PublishBlockedHandle> publishBlockedHandle
) {
  // SUBSCRIBE_TRACKS is namespace-level, like SUBSCRIBE_NAMESPACE; gate on the
  // prefix. Unlike peering, which uses SUBSCRIBE_NAMESPACE, there is no peer
  // bypass for SUBSCRIBE_TRACKS.
  auto ok = auth::authorize(
      *verifier_,
      auth::Action::SubscribeNamespace,
      subTracks.params,
      subTracks.trackNamespacePrefix,
      *grants_
  );
  if (ok.hasError()) {
    return folly::coro::makeTask<Publisher::SubscribeTracksResult>(
        folly::makeUnexpected(SubscribeTracksError{
            subTracks.requestID,
            SubscribeTracksErrorCode::UNAUTHORIZED,
            auth::toString(ok.error())
        })
    );
  }
  return downstream_->subscribeTracks(std::move(subTracks), std::move(publishBlockedHandle));
}

folly::coro::Task<Publisher::TrackStatusResult>
AuthPublisherFilter::trackStatus(const TrackStatus req) {
  auto ok = auth::authorize(
      *verifier_,
      auth::Action::TrackStatus,
      req.params,
      req.fullTrackName.trackNamespace,
      *grants_,
      req.fullTrackName.trackName
  );
  if (ok.hasError()) {
    return folly::coro::makeTask<Publisher::TrackStatusResult>(
        folly::makeUnexpected(TrackStatusError{
            req.requestID,
            TrackStatusErrorCode::UNAUTHORIZED,
            auth::toString(ok.error())
        })
    );
  }
  return downstream_->trackStatus(req);
}

void AuthPublisherFilter::goaway(Goaway g) {
  downstream_->goaway(std::move(g));
}

Subscriber::PublishResult
AuthSubscriberFilter::publish(PublishRequest pub, std::shared_ptr<SubscriptionHandle> handle) {
  auto ok = auth::authorize(
      *verifier_,
      auth::Action::Publish,
      pub.params,
      pub.fullTrackName.trackNamespace,
      *grants_,
      pub.fullTrackName.trackName
  );
  if (ok.hasError()) {
    return folly::makeUnexpected(
        PublishError{pub.requestID, PublishErrorCode::UNAUTHORIZED, auth::toString(ok.error())}
    );
  }
  return downstream_->publish(std::move(pub), std::move(handle));
}

folly::coro::Task<Subscriber::PublishNamespaceResult> AuthSubscriberFilter::publishNamespace(
    PublishNamespace pubNs,
    std::shared_ptr<PublishNamespaceCallback> callback
) {
  auto ok = auth::authorize(
      *verifier_,
      auth::Action::PublishNamespace,
      pubNs.params,
      pubNs.trackNamespace,
      *grants_
  );
  if (ok.hasError()) {
    return folly::coro::makeTask<Subscriber::PublishNamespaceResult>(
        folly::makeUnexpected(PublishNamespaceError{
            pubNs.requestID,
            PublishNamespaceErrorCode::UNAUTHORIZED,
            auth::toString(ok.error())
        })
    );
  }
  return downstream_->publishNamespace(std::move(pubNs), std::move(callback));
}

void AuthSubscriberFilter::goaway(Goaway g) {
  downstream_->goaway(std::move(g));
}

} // namespace openmoq::moqx
