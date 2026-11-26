/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/AuthFilters.h"

#include "peering/PeeringBackend.h"
#include "relay/NamespaceRevocable.h"

#include <moxygen/MoQConsumers.h>

#include <folly/Function.h>
#include <folly/coro/Task.h>
#include <quic/common/events/QuicEventBase.h>

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
  uint64_t connectionId{0};

  folly::Expected<auth::AuthDecision, auth::AuthError> check(const RequestUpdate& update) const {
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
        trackName ? std::optional<std::string_view>(*trackName) : std::nullopt,
        connectionId
    );
  }
};

// Without an authorization, updates pass through unchecked: a PUBLISH handle carries the relay's
// updates to the publisher, which are not the publisher's to authorize.
template <typename Handle> class AuthUpdateHandle : public Handle, private quic::QuicTimerCallback {
public:
  AuthUpdateHandle(std::shared_ptr<Handle> inner, std::optional<UpdateAuthorization> authorization)
      : inner_(std::move(inner)), authorization_(std::move(authorization)) {}

  folly::coro::Task<typename Handle::RequestUpdateResult> requestUpdate(RequestUpdate update
  ) override {
    if (authorization_) {
      const auto authorized = authorization_->check(update);
      if (authorized.hasError()) {
        co_return folly::makeUnexpected(RequestError{
            update.requestID,
            requestErrorFor(authorized.error()),
            auth::toString(authorized.error())
        });
      }
    }
    co_return co_await inner_->requestUpdate(std::move(update));
  }

  // Ends the action through endAction at the token's deadline, unless the peer ends it first.
  // Call on exec. The timer restores the current request context, through which moxygen finds the
  // session that owns a handle (MoQSession::getRequestSession); without it the relay ignores the
  // call.
  void expireAt(
      MoQExecutor* exec,
      const char* action,
      const std::optional<auth::Revalidation>& revalidation,
      folly::Function<void(Handle&)> endAction
  ) {
    if (!revalidation) {
      return;
    }
    action_ = action;
    endAction_ = std::move(endAction);
    exec->scheduleTimeout(
        this,
        std::max(
            std::chrono::milliseconds(0),
            std::chrono::ceil<std::chrono::milliseconds>(
                revalidation->deadline - std::chrono::system_clock::now()
            )
        )
    );
  }

protected:
  // Call when the peer ends the action. True when revalidation already ended it, so the inner
  // handle must not be torn down a second time.
  bool endedByRevalidation() {
    cancelTimerCallback();
    return expired_;
  }

  std::shared_ptr<Handle> inner_;

private:
  void timeoutExpired() noexcept override {
    expired_ = true;
    // endAction can destroy this handle (a PUBLISH_NAMESPACE cancel does), so run it from locals.
    auto inner = inner_;
    auto endAction = std::exchange(endAction_, nullptr);
    XLOG(INFO) << "revalidation: the token authorizing a " << action_
               << " is no longer acceptable; ending it";
    try {
      endAction(*inner);
    } catch (const std::exception& e) {
      XLOG(WARN) << "revalidation: ending the action failed: " << e.what();
    }
  }
  // The default runs timeoutExpired, which would end the action when the timer is torn down.
  void callbackCanceled() noexcept override {}

  std::optional<UpdateAuthorization> authorization_;
  const char* action_{""};
  folly::Function<void(Handle&)> endAction_;
  bool expired_{false};
};

class AuthSubscriptionHandle final : public AuthUpdateHandle<SubscriptionHandle> {
public:
  AuthSubscriptionHandle(
      std::shared_ptr<SubscriptionHandle> inner,
      std::optional<UpdateAuthorization> authorization
  )
      : AuthUpdateHandle(std::move(inner), std::move(authorization)) {
    if (inner_->hasSubscribeOk()) {
      setSubscribeOk(inner_->subscribeOk());
    }
  }
  void unsubscribe() override {
    if (!endedByRevalidation()) {
      inner_->unsubscribe();
    }
  }
  void cancelFill(RequestID id) override { inner_->cancelFill(id); }
  const SubscribeOk& subscribeOk() const override { return inner_->subscribeOk(); }
};

class AuthFetchHandle final : public AuthUpdateHandle<Publisher::FetchHandle> {
public:
  using AuthUpdateHandle::AuthUpdateHandle;
  void fetchCancel() override {
    if (!endedByRevalidation()) {
      inner_->fetchCancel();
    }
  }
  const FetchOk& fetchOk() const override { return inner_->fetchOk(); }
};

class AuthSubscribeNamespaceHandle final
    : public AuthUpdateHandle<Publisher::SubscribeNamespaceHandle> {
public:
  using AuthUpdateHandle::AuthUpdateHandle;
  void unsubscribeNamespace() override {
    if (!endedByRevalidation()) {
      inner_->unsubscribeNamespace();
    }
  }
  const SubscribeNamespaceOk& subscribeNamespaceOk() const override {
    return inner_->subscribeNamespaceOk();
  }
};

class AuthSubscribeTracksHandle final : public AuthUpdateHandle<Publisher::SubscribeTracksHandle> {
public:
  using AuthUpdateHandle::AuthUpdateHandle;
  void unsubscribeTracks() override {
    if (!endedByRevalidation()) {
      inner_->unsubscribeTracks();
    }
  }
  const SubscribeTracksOk& subscribeTracksOk() const override {
    return inner_->subscribeTracksOk();
  }
};

class AuthPublishNamespaceHandle final
    : public AuthUpdateHandle<Subscriber::PublishNamespaceHandle> {
public:
  using AuthUpdateHandle::AuthUpdateHandle;
  void publishNamespaceDone() override {
    if (!endedByRevalidation()) {
      inner_->publishNamespaceDone();
    }
  }
  const PublishNamespaceOk& publishNamespaceOk() const override {
    return inner_->publishNamespaceOk();
  }
};

using PublishReply = folly::coro::Task<folly::Expected<PublishOk, PublishError>>;

// PUBLISH is synchronous and may hand its handle to the relay's executor, which drops it there
// when it refuses the PUBLISH. The timer starts only after PUBLISH_OK, in the reply the session
// awaits on exec; by then the relay holds the handle bound to exec.
PublishReply expireAfterPublishOk(
    PublishReply reply,
    MoQExecutor* exec,
    std::weak_ptr<AuthSubscriptionHandle> weakHandle,
    auth::Revalidation revalidation
) {
  auto result = co_await std::move(reply);
  if (result.hasValue()) {
    if (auto handle = weakHandle.lock()) {
      handle->expireAt(exec, "publish", revalidation, [](SubscriptionHandle& inner) {
        // The relay ends the publisher's track with an unsubscribe; other actions are untouched.
        inner.unsubscribe();
      });
    }
  }
  co_return result;
}

} // namespace

folly::coro::Task<Publisher::SubscribeResult>
AuthPublisherFilter::subscribe(SubscribeRequest sub, std::shared_ptr<TrackConsumer> consumer) {
  auto ok = auth::authorize(
      *verifier_,
      auth::Action::Subscribe,
      sub.params,
      sub.fullTrackName.trackNamespace,
      *grants_,
      sub.fullTrackName.trackName,
      connectionId_
  );
  if (ok.hasError()) {
    co_return folly::makeUnexpected(
        SubscribeError{sub.requestID, requestErrorFor(ok.error()), auth::toString(ok.error())}
    );
  }
  UpdateAuthorization target{
      verifier_,
      grants_,
      sub.fullTrackName.trackNamespace,
      sub.fullTrackName.trackName,
      connectionId_
  };
  const auto requestID = sub.requestID;
  std::weak_ptr<TrackConsumer> weakConsumer = consumer;
  auto result = co_await downstream_->subscribe(std::move(sub), std::move(consumer));
  if (result.hasValue()) {
    auto handle =
        std::make_shared<AuthSubscriptionHandle>(std::move(result.value()), std::move(target));
    handle->expireAt(
        exec_,
        "subscription",
        ok.value().revalidation,
        [weakConsumer, requestID](SubscriptionHandle& inner) {
          // Only this subscription ends; the session and its other actions are untouched.
          auto consumer = weakConsumer.lock();
          inner.unsubscribe();
          if (consumer) {
            (void)consumer->publishDone(PublishDone{
                requestID,
                // draft 18: "The subscriber is no longer authorized to subscribe to the given
                // track". PublishDoneStatusCode::EXPIRED means the SUBSCRIBE_OK timeout.
                PublishDoneStatusCode::UNAUTHORIZED,
                0,
                "authorization token no longer acceptable"
            });
          }
        }
    );
    result.value() = std::move(handle);
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
      fetch.fullTrackName.trackName,
      connectionId_
  );
  if (ok.hasError()) {
    co_return folly::makeUnexpected(
        FetchError{fetch.requestID, requestErrorFor(ok.error()), auth::toString(ok.error())}
    );
  }
  UpdateAuthorization target{
      verifier_,
      grants_,
      fetch.fullTrackName.trackNamespace,
      fetch.fullTrackName.trackName,
      connectionId_
  };
  std::weak_ptr<FetchConsumer> weakConsumer = consumer;
  auto result = co_await downstream_->fetch(std::move(fetch), std::move(consumer));
  if (result.hasValue()) {
    auto handle = std::make_shared<AuthFetchHandle>(std::move(result.value()), std::move(target));
    handle->expireAt(
        exec_,
        "fetch",
        ok.value().revalidation,
        [weakConsumer](Publisher::FetchHandle& inner) {
          auto consumer = weakConsumer.lock();
          inner.fetchCancel();
          if (consumer) {
            consumer->reset(ResetStreamErrorCode::EXPIRED_AUTH_TOKEN);
          }
        }
    );
    result.value() = std::move(handle);
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
  std::optional<auth::Revalidation> revalidation;
  if (!isPeer) {
    auto ok = auth::authorize(
        *verifier_,
        auth::Action::SubscribeNamespace,
        subNs.params,
        subNs.trackNamespacePrefix,
        *grants_,
        std::nullopt,
        connectionId_
    );
    if (ok.hasError()) {
      co_return folly::makeUnexpected(SubscribeNamespaceError{
          subNs.requestID,
          requestErrorFor(ok.error()),
          auth::toString(ok.error())
      });
    }
    revalidation = ok.value().revalidation;
  }
  UpdateAuthorization
      target{verifier_, grants_, subNs.trackNamespacePrefix, std::nullopt, connectionId_};
  auto result = co_await downstream_->subscribeNamespace(std::move(subNs), std::move(handle));
  if (result.hasValue() && !isPeer) {
    auto wrapped = std::make_shared<AuthSubscribeNamespaceHandle>(
        std::move(result.value()),
        std::move(target)
    );
    wrapped->expireAt(
        exec_,
        "subscribe_namespace",
        revalidation,
        [](Publisher::SubscribeNamespaceHandle& inner) {
          // Stops the NAMESPACE flow for this one subscription.
          inner.unsubscribeNamespace();
        }
    );
    result.value() = std::move(wrapped);
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
      *grants_,
      std::nullopt,
      connectionId_
  );
  if (ok.hasError()) {
    co_return folly::makeUnexpected(SubscribeTracksError{
        subTracks.requestID,
        requestErrorFor(ok.error()),
        auth::toString(ok.error())
    });
  }
  UpdateAuthorization
      target{verifier_, grants_, subTracks.trackNamespacePrefix, std::nullopt, connectionId_};
  auto result =
      co_await downstream_->subscribeTracks(std::move(subTracks), std::move(publishBlockedHandle));
  if (result.hasValue()) {
    auto wrapped =
        std::make_shared<AuthSubscribeTracksHandle>(std::move(result.value()), std::move(target));
    wrapped->expireAt(
        exec_,
        "subscribe_tracks",
        ok.value().revalidation,
        [](Publisher::SubscribeTracksHandle& inner) { inner.unsubscribeTracks(); }
    );
    result.value() = std::move(wrapped);
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
      req.fullTrackName.trackName,
      connectionId_
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
      pub.fullTrackName.trackName,
      connectionId_
  );
  if (ok.hasError()) {
    return folly::makeUnexpected(
        PublishError{pub.requestID, requestErrorFor(ok.error()), auth::toString(ok.error())}
    );
  }
  if (!ok.value().revalidation) {
    return downstream_->publish(std::move(pub), std::move(handle));
  }
  auto wrapped = std::make_shared<AuthSubscriptionHandle>(std::move(handle), std::nullopt);
  std::weak_ptr<AuthSubscriptionHandle> weakWrapped = wrapped;
  auto result = downstream_->publish(std::move(pub), std::move(wrapped));
  if (result.hasValue()) {
    result.value().reply = expireAfterPublishOk(
        std::move(result.value().reply),
        exec_,
        std::move(weakWrapped),
        *ok.value().revalidation
    );
  }
  return result;
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
      *grants_,
      std::nullopt,
      connectionId_
  );
  if (ok.hasError()) {
    co_return folly::makeUnexpected(PublishNamespaceError{
        pubNs.requestID,
        requestErrorFor(ok.error()),
        auth::toString(ok.error())
    });
  }
  UpdateAuthorization target{verifier_, grants_, pubNs.trackNamespace, std::nullopt, connectionId_};
  std::weak_ptr<PublishNamespaceCallback> weakCallback = callback;
  auto result = co_await downstream_->publishNamespace(std::move(pubNs), std::move(callback));
  if (result.hasValue()) {
    auto wrapped =
        std::make_shared<AuthPublishNamespaceHandle>(std::move(result.value()), std::move(target));
    wrapped->expireAt(
        exec_,
        "publish_namespace",
        ok.value().revalidation,
        [weakCallback](Subscriber::PublishNamespaceHandle& inner) {
          // Drop the route first: publishNamespaceCancel makes the session erase its entry for
          // this namespace, which destroys the handle and would leave the relay's route behind.
          // Then tell the publisher its namespace was withdrawn.
          auto callback = weakCallback.lock();
          // Also ends the tracks the publisher is already sending under the namespace.
          if (auto* revocable = dynamic_cast<NamespaceRevocable*>(&inner)) {
            revocable->revokeNamespace();
          } else {
            inner.publishNamespaceDone();
          }
          if (callback) {
            callback->publishNamespaceCancel(
                PublishNamespaceErrorCode::EXPIRED_AUTH_TOKEN,
                "authorization token no longer acceptable"
            );
          }
        }
    );
    result.value() = std::move(wrapped);
  }
  co_return result;
}

void AuthSubscriberFilter::goaway(Goaway g) {
  downstream_->goaway(std::move(g));
}

} // namespace openmoq::moqx
