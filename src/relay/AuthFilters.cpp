/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/AuthFilters.h"

#include "peering/PeeringBackend.h"

#include <moxygen/MoQConsumers.h>

using namespace moxygen;

namespace openmoq::moqx {
namespace {

// Draft 18, section 10.2.2: a message that carries an expired token fails with
// EXPIRED_AUTH_TOKEN, and one whose token cannot be decoded fails with MALFORMED_AUTH_TOKEN. Every
// other denial (no token, a bad signature, an unsupported claim, a token that does not cover the
// action) stays UNAUTHORIZED. Request error codes go out on the wire unchanged in every draft, so
// these are also the codes for drafts 16 and 22.
RequestErrorCode requestErrorFor(auth::AuthError error) {
  switch (error) {
  case auth::AuthError::Expired:
    return RequestErrorCode::EXPIRED_AUTH_TOKEN;
  case auth::AuthError::Malformed:
    return RequestErrorCode::MALFORMED_AUTH_TOKEN;
  default:
    return RequestErrorCode::UNAUTHORIZED;
  }
}

struct UpdateAuthorization {
  std::shared_ptr<const auth::AuthTokenVerifier> verifier;
  std::shared_ptr<const std::vector<auth::Grants>> grants;
  TrackNamespace trackNamespace;
  std::optional<std::string> trackName;

  folly::Expected<folly::Unit, auth::AuthError> check(const RequestUpdate& update) const {
    if (verifier->enabled()) {
      for (const auto& param : update.params) {
        // The core currently authorizes a fixed target. A prefix-changing update
        // needs checks on both old and new targets before it can be supported.
        if (param.key == static_cast<uint64_t>(TrackRequestParamKey::TRACK_NAMESPACE_PREFIX)) {
          return folly::makeUnexpected(auth::AuthError::UnsupportedClaim);
        }
      }
    }
    return auth::authorize(
        *verifier,
        auth::Action::RequestUpdate,
        update.params,
        trackNamespace,
        *grants,
        trackName ? std::optional<std::string_view>(*trackName) : std::nullopt
    );
  }
};

template <typename Handle> class AuthUpdateHandle : public Handle {
public:
  AuthUpdateHandle(std::shared_ptr<Handle> inner, UpdateAuthorization authorization)
      : inner_(std::move(inner)), authorization_(std::move(authorization)) {}

  folly::coro::Task<typename Handle::RequestUpdateResult> requestUpdate(RequestUpdate update
  ) override {
    const auto authorized = authorization_.check(update);
    if (authorized.hasError()) {
      co_return folly::makeUnexpected(RequestError{
          update.requestID,
          requestErrorFor(authorized.error()),
          auth::toString(authorized.error())
      });
    }
    co_return co_await inner_->requestUpdate(std::move(update));
  }

protected:
  std::shared_ptr<Handle> inner_;

private:
  UpdateAuthorization authorization_;
};

class AuthSubscriptionHandle final : public AuthUpdateHandle<SubscriptionHandle> {
public:
  AuthSubscriptionHandle(
      std::shared_ptr<SubscriptionHandle> inner,
      UpdateAuthorization authorization
  )
      : AuthUpdateHandle(std::move(inner), std::move(authorization)) {
    if (inner_->hasSubscribeOk()) {
      setSubscribeOk(inner_->subscribeOk());
    }
  }
  void unsubscribe() override { inner_->unsubscribe(); }
  void cancelFill(RequestID id) override { inner_->cancelFill(id); }
  const SubscribeOk& subscribeOk() const override { return inner_->subscribeOk(); }
};

class AuthFetchHandle final : public AuthUpdateHandle<Publisher::FetchHandle> {
public:
  using AuthUpdateHandle::AuthUpdateHandle;
  void fetchCancel() override { inner_->fetchCancel(); }
  const FetchOk& fetchOk() const override { return inner_->fetchOk(); }
};

class AuthSubscribeNamespaceHandle final
    : public AuthUpdateHandle<Publisher::SubscribeNamespaceHandle> {
public:
  using AuthUpdateHandle::AuthUpdateHandle;
  void unsubscribeNamespace() override { inner_->unsubscribeNamespace(); }
  const SubscribeNamespaceOk& subscribeNamespaceOk() const override {
    return inner_->subscribeNamespaceOk();
  }
};

class AuthSubscribeTracksHandle final : public AuthUpdateHandle<Publisher::SubscribeTracksHandle> {
public:
  using AuthUpdateHandle::AuthUpdateHandle;
  void unsubscribeTracks() override { inner_->unsubscribeTracks(); }
  const SubscribeTracksOk& subscribeTracksOk() const override {
    return inner_->subscribeTracksOk();
  }
};

class AuthPublishNamespaceHandle final
    : public AuthUpdateHandle<Subscriber::PublishNamespaceHandle> {
public:
  using AuthUpdateHandle::AuthUpdateHandle;
  void publishNamespaceDone() override { inner_->publishNamespaceDone(); }
  const PublishNamespaceOk& publishNamespaceOk() const override {
    return inner_->publishNamespaceOk();
  }
};

} // namespace

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
    co_return folly::makeUnexpected(
        SubscribeError{sub.requestID, requestErrorFor(ok.error()), auth::toString(ok.error())}
    );
  }
  UpdateAuthorization
      target{verifier_, grants_, sub.fullTrackName.trackNamespace, sub.fullTrackName.trackName};
  auto result = co_await downstream_->subscribe(std::move(sub), std::move(consumer));
  if (result.hasValue()) {
    result.value() =
        std::make_shared<AuthSubscriptionHandle>(std::move(result.value()), std::move(target));
  }
  co_return result;
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
    co_return folly::makeUnexpected(
        FetchError{fetch.requestID, requestErrorFor(ok.error()), auth::toString(ok.error())}
    );
  }
  UpdateAuthorization
      target{verifier_, grants_, fetch.fullTrackName.trackNamespace, fetch.fullTrackName.trackName};
  auto result = co_await downstream_->fetch(std::move(fetch), std::move(consumer));
  if (result.hasValue()) {
    result.value() =
        std::make_shared<AuthFetchHandle>(std::move(result.value()), std::move(target));
  }
  co_return result;
}

folly::coro::Task<Publisher::SubscribeNamespaceResult> AuthPublisherFilter::subscribeNamespace(
    SubscribeNamespace subNs,
    std::shared_ptr<NamespacePublishHandle> handle
) {
  // Peer relays authenticate via their relay token, validated downstream;
  // only non-peer subscribers go through grant-based auth here.
  bool isPeer = reciprocal_ && reciprocal_->authorizePeer(subNs.params).has_value();
  if (!isPeer) {
    auto ok = auth::authorize(
        *verifier_,
        auth::Action::SubscribeNamespace,
        subNs.params,
        subNs.trackNamespacePrefix,
        *grants_
    );
    if (ok.hasError()) {
      co_return folly::makeUnexpected(SubscribeNamespaceError{
          subNs.requestID,
          requestErrorFor(ok.error()),
          auth::toString(ok.error())
      });
    }
  }
  UpdateAuthorization target{verifier_, grants_, subNs.trackNamespacePrefix, std::nullopt};
  auto result = co_await downstream_->subscribeNamespace(std::move(subNs), std::move(handle));
  if (result.hasValue() && !isPeer) {
    result.value() = std::make_shared<AuthSubscribeNamespaceHandle>(
        std::move(result.value()),
        std::move(target)
    );
  }
  co_return result;
}

folly::coro::Task<Publisher::SubscribeTracksResult> AuthPublisherFilter::subscribeTracks(
    SubscribeTracks subTracks,
    std::shared_ptr<PublishBlockedHandle> publishBlockedHandle
) {
  // C4M gives SUBSCRIBE_TRACKS its own action, distinct from SUBSCRIBE_NAMESPACE.
  // Peer relays use their separate relay-token policy.
  if (reciprocal_ && reciprocal_->authorizePeer(subTracks.params).has_value()) {
    co_return co_await downstream_->subscribeTracks(
        std::move(subTracks),
        std::move(publishBlockedHandle)
    );
  }
  auto ok = auth::authorize(
      *verifier_,
      auth::Action::SubscribeTracks,
      subTracks.params,
      subTracks.trackNamespacePrefix,
      *grants_
  );
  if (ok.hasError()) {
    co_return folly::makeUnexpected(SubscribeTracksError{
        subTracks.requestID,
        requestErrorFor(ok.error()),
        auth::toString(ok.error())
    });
  }
  UpdateAuthorization target{verifier_, grants_, subTracks.trackNamespacePrefix, std::nullopt};
  auto result =
      co_await downstream_->subscribeTracks(std::move(subTracks), std::move(publishBlockedHandle));
  if (result.hasValue()) {
    result.value() =
        std::make_shared<AuthSubscribeTracksHandle>(std::move(result.value()), std::move(target));
  }
  co_return result;
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
    return folly::coro::makeTask<Publisher::TrackStatusResult>(folly::makeUnexpected(
        TrackStatusError{req.requestID, requestErrorFor(ok.error()), auth::toString(ok.error())}
    ));
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
        PublishError{pub.requestID, requestErrorFor(ok.error()), auth::toString(ok.error())}
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
    co_return folly::makeUnexpected(PublishNamespaceError{
        pubNs.requestID,
        requestErrorFor(ok.error()),
        auth::toString(ok.error())
    });
  }
  UpdateAuthorization target{verifier_, grants_, pubNs.trackNamespace, std::nullopt};
  auto result = co_await downstream_->publishNamespace(std::move(pubNs), std::move(callback));
  if (result.hasValue()) {
    result.value() =
        std::make_shared<AuthPublishNamespaceHandle>(std::move(result.value()), std::move(target));
  }
  co_return result;
}

void AuthSubscriberFilter::goaway(Goaway g) {
  downstream_->goaway(std::move(g));
}

} // namespace openmoq::moqx
