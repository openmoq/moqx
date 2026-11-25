/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/AuthFilters.h"

#include "peering/PeeringBackend.h"
#include "relay/NamespaceRevocable.h"

#include <moxygen/MoQConsumers.h>

#include <folly/CancellationToken.h>
#include <folly/coro/Invoke.h>
#include <folly/coro/Sleep.h>
#include <folly/coro/Task.h>
#include <folly/coro/Timeout.h>
#include <folly/coro/WithCancellation.h>
#include <folly/futures/Future.h>
#include <folly/io/async/Request.h>

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

// One ongoing action authorized by a token with moqt-reval. Destroying it (the action ended
// normally) cancels the revalidation loop.
struct ActionMonitor {
  folly::CancellationSource cancel;
  // The loop already ended the action, so the normal teardown must not repeat it.
  bool ended{false};
  // The request context of the message that started the action. moxygen finds the session that
  // owns a handle through it (MoQSession::getRequestSession), so ending the action from a timer
  // has to run inside the same context or the relay ignores the call.
  std::shared_ptr<folly::RequestContext> requestContext;
  ~ActionMonitor() { cancel.requestCancellation(); }
};

using EndAction = std::function<void(ActionMonitor&)>;

// Re-checks the authorizing token every `interval` and, once it is no longer acceptable (its
// latest expiry has passed), ends only this action through `endAction`.
//
// `ongoing` (optional) says whether the action is still there; the loop stops once it is not.
// `keepAlive` (optional) is a strong reference to the monitor for actions that have no handle to
// own it, such as PUBLISH: the loop then owns the monitor until the action ends.
folly::coro::Task<void> revalidationLoop(
    const char* action,
    auth::Revalidation revalidation,
    std::weak_ptr<ActionMonitor> weakMonitor,
    folly::CancellationToken token,
    EndAction endAction,
    std::function<bool()> ongoing,
    std::shared_ptr<ActionMonitor> keepAlive
) {
  for (;;) {
    const auto now = std::chrono::system_clock::now();
    if (now >= revalidation.deadline) {
      if (auto monitor = weakMonitor.lock(); monitor && !monitor->ended) {
        monitor->ended = true;
        XLOG(INFO) << "revalidation: the token authorizing a " << action
                   << " is no longer acceptable; ending it";
        try {
          folly::RequestContextScopeGuard guard(monitor->requestContext);
          endAction(*monitor);
        } catch (const std::exception& e) {
          XLOG(WARN) << "revalidation: ending the action failed: " << e.what();
        }
      }
      co_return;
    }
    const auto wait = std::min<std::chrono::system_clock::duration>(
        revalidation.interval,
        revalidation.deadline - now
    );
    auto slept = co_await folly::coro::co_awaitTry(folly::coro::co_withCancellation(
        token,
        folly::coro::sleep(
            std::chrono::duration_cast<std::chrono::milliseconds>(wait) +
            std::chrono::milliseconds(1)
        )
    ));
    if (slept.hasException() || !weakMonitor.lock() || (ongoing && !ongoing())) {
      co_return; // The action ended on its own.
    }
  }
}

// Returns null when no revalidation applies. With `ongoing`, the loop owns the monitor and runs
// until the action is gone (see revalidationLoop); the returned pointer is then only a handle to
// cancel it early.
std::shared_ptr<ActionMonitor> startRevalidation(
    const char* action,
    const std::optional<auth::Revalidation>& revalidation,
    folly::Executor* executor,
    EndAction endAction,
    std::function<bool()> ongoing = nullptr,
    std::shared_ptr<folly::RequestContext> requestContext = nullptr
) {
  if (!revalidation) {
    return nullptr;
  }
  if (executor == nullptr) {
    XLOG(WARN) << "revalidation: no executor for a " << action << "; it will not be revalidated";
    return nullptr;
  }
  XLOG(DBG1) << "revalidation: watching a " << action << ", every "
             << revalidation->interval.count() << "s";
  auto monitor = std::make_shared<ActionMonitor>();
  monitor->requestContext =
      requestContext ? std::move(requestContext) : folly::RequestContext::saveContext();
  folly::coro::co_withExecutor(
      folly::getKeepAliveToken(executor),
      revalidationLoop(
          action,
          *revalidation,
          monitor,
          monitor->cancel.getToken(),
          std::move(endAction),
          ongoing,
          ongoing ? monitor : nullptr
      )
  )
      .start();
  return monitor;
}

using PublishReply = folly::coro::Task<folly::Expected<PublishOk, PublishError>>;

// The reply of an accepted PUBLISH is awaited on the session's executor, which is the only place
// PUBLISH (a synchronous call) gives us one. Start the timer there, and drop it if the PUBLISH is
// refused.
PublishReply revalidatedPublishReply(
    PublishReply inner,
    std::optional<auth::Revalidation> revalidation,
    std::weak_ptr<SubscriptionHandle> weakHandle,
    std::weak_ptr<TrackConsumer> weakConsumer,
    std::shared_ptr<folly::RequestContext> requestContext
) {
  auto* executor = co_await folly::coro::co_current_executor;
  auto monitor = startRevalidation(
      "publish",
      revalidation,
      executor,
      [weakHandle](ActionMonitor&) {
        // The relay ends the publisher's track with an unsubscribe; other actions are untouched.
        if (auto handle = weakHandle.lock()) {
          handle->unsubscribe();
        }
      },
      [weakHandle, weakConsumer] { return !(weakHandle.expired() && weakConsumer.expired()); },
      std::move(requestContext)
  );
  auto result = co_await std::move(inner);
  if (monitor && result.hasError()) {
    monitor->cancel.requestCancellation(); // Never became an ongoing publish.
  }
  co_return result;
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

  void attachMonitor(std::shared_ptr<ActionMonitor> monitor) { monitor_ = std::move(monitor); }

protected:
  // Call when the peer ends the action. True when revalidation already ended it, so the inner
  // handle must not be torn down a second time.
  bool endedByRevalidation() {
    if (!monitor_) {
      return false;
    }
    monitor_->cancel.requestCancellation();
    return monitor_->ended;
  }

  std::shared_ptr<Handle> inner_;

private:
  UpdateAuthorization authorization_;
  std::shared_ptr<ActionMonitor> monitor_;
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
  auto* executor = co_await folly::coro::co_current_executor;
  const auto requestID = sub.requestID;
  std::weak_ptr<TrackConsumer> weakConsumer = consumer;
  auto result = co_await downstream_->subscribe(std::move(sub), std::move(consumer));
  if (result.hasValue()) {
    std::weak_ptr<SubscriptionHandle> weakInner = result.value();
    auto handle =
        std::make_shared<AuthSubscriptionHandle>(std::move(result.value()), std::move(target));
    handle->attachMonitor(startRevalidation(
        "subscription",
        ok.value().revalidation,
        executor,
        [weakInner, weakConsumer, requestID](ActionMonitor&) {
          // Only this subscription ends; the session and its other actions are untouched.
          auto consumer = weakConsumer.lock();
          if (auto inner = weakInner.lock()) {
            inner->unsubscribe();
          }
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
    ));
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
  auto* executor = co_await folly::coro::co_current_executor;
  std::weak_ptr<FetchConsumer> weakConsumer = consumer;
  auto result = co_await downstream_->fetch(std::move(fetch), std::move(consumer));
  if (result.hasValue()) {
    std::weak_ptr<Publisher::FetchHandle> weakInner = result.value();
    auto handle = std::make_shared<AuthFetchHandle>(std::move(result.value()), std::move(target));
    handle->attachMonitor(startRevalidation(
        "fetch",
        ok.value().revalidation,
        executor,
        [weakInner, weakConsumer](ActionMonitor&) {
          auto consumer = weakConsumer.lock();
          if (auto inner = weakInner.lock()) {
            inner->fetchCancel();
          }
          if (consumer) {
            consumer->reset(ResetStreamErrorCode::EXPIRED_AUTH_TOKEN);
          }
        }
    ));
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
  auto* executor = co_await folly::coro::co_current_executor;
  auto result = co_await downstream_->subscribeNamespace(std::move(subNs), std::move(handle));
  if (result.hasValue() && !isPeer) {
    std::weak_ptr<Publisher::SubscribeNamespaceHandle> weakInner = result.value();
    auto wrapped = std::make_shared<AuthSubscribeNamespaceHandle>(
        std::move(result.value()),
        std::move(target)
    );
    wrapped->attachMonitor(startRevalidation(
        "subscribe_namespace",
        revalidation,
        executor,
        [weakInner](ActionMonitor&) {
          // Stops the NAMESPACE flow for this one subscription.
          if (auto inner = weakInner.lock()) {
            inner->unsubscribeNamespace();
          }
        }
    ));
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
  auto* executor = co_await folly::coro::co_current_executor;
  auto result =
      co_await downstream_->subscribeTracks(std::move(subTracks), std::move(publishBlockedHandle));
  if (result.hasValue()) {
    std::weak_ptr<Publisher::SubscribeTracksHandle> weakInner = result.value();
    auto wrapped =
        std::make_shared<AuthSubscribeTracksHandle>(std::move(result.value()), std::move(target));
    wrapped->attachMonitor(startRevalidation(
        "subscribe_tracks",
        ok.value().revalidation,
        executor,
        [weakInner](ActionMonitor&) {
          if (auto inner = weakInner.lock()) {
            inner->unsubscribeTracks();
          }
        }
    ));
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
  std::weak_ptr<SubscriptionHandle> weakHandle = handle;
  auto result = downstream_->publish(std::move(pub), std::move(handle));
  if (result.hasValue() && ok.value().revalidation) {
    std::weak_ptr<TrackConsumer> weakConsumer = result.value().consumer;
    result.value().reply = revalidatedPublishReply(
        std::move(result.value().reply),
        ok.value().revalidation,
        std::move(weakHandle),
        std::move(weakConsumer),
        folly::RequestContext::saveContext()
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
  auto* executor = co_await folly::coro::co_current_executor;
  std::weak_ptr<PublishNamespaceCallback> weakCallback = callback;
  auto result = co_await downstream_->publishNamespace(std::move(pubNs), std::move(callback));
  if (result.hasValue()) {
    std::weak_ptr<Subscriber::PublishNamespaceHandle> weakInner = result.value();
    auto wrapped =
        std::make_shared<AuthPublishNamespaceHandle>(std::move(result.value()), std::move(target));
    wrapped->attachMonitor(startRevalidation(
        "publish_namespace",
        ok.value().revalidation,
        executor,
        [weakInner, weakCallback](ActionMonitor&) {
          // Drop the route first: publishNamespaceCancel makes the session erase its entry for
          // this namespace, which destroys the handle and would leave the relay's route behind.
          // Then tell the publisher its namespace was withdrawn.
          auto callback = weakCallback.lock();
          if (auto inner = weakInner.lock()) {
            // Also ends the tracks the publisher is already sending under the namespace.
            if (auto revocable = std::dynamic_pointer_cast<NamespaceRevocable>(inner)) {
              revocable->revokeNamespace();
            } else {
              inner->publishNamespaceDone();
            }
          }
          if (callback) {
            callback->publishNamespaceCancel(
                PublishNamespaceErrorCode::EXPIRED_AUTH_TOKEN,
                "authorization token no longer acceptable"
            );
          }
        }
    ));
    result.value() = std::move(wrapped);
  }
  co_return result;
}

void AuthSubscriberFilter::goaway(Goaway g) {
  downstream_->goaway(std::move(g));
}

} // namespace openmoq::moqx
