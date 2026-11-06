/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/SessionParticipant.h"

#include "relay/ExecutorCheck.h"
#include "relay/PublisherCrossExecFilter.h"
#include "relay/SubscriberCrossExecFilter.h"

#include <folly/logging/xlog.h>

#include <utility>

namespace openmoq::moqx {

namespace {

constexpr std::string_view kSessionClosed = "session closed";

// Must lock on the session's executor, so another thread never holds the last reference.
class WeakSession {
public:
  explicit WeakSession(const std::shared_ptr<moxygen::MoQSession>& session)
      : session_(session), exec_(session->getExecutor()) {}

  std::shared_ptr<moxygen::MoQSession> lock() const {
    XDCHECK(onExecutor(exec_)) << "session locked off its executor";
    return session_.lock();
  }

private:
  std::weak_ptr<moxygen::MoQSession> session_;
  folly::Executor* exec_;
};

// The task owns its WeakSession, so it does not need the proxy that started it.
template <typename Result, typename Error, typename Fn>
folly::coro::Task<Result> withSession(WeakSession weak, Error closed, Fn call) {
  auto session = weak.lock();
  if (!session) {
    co_return folly::makeUnexpected(std::move(closed));
  }
  co_return co_await call(*session);
}

// Calls into a session that the relay must not keep alive.
class WeakSessionPublisher : public moxygen::Publisher {
public:
  explicit WeakSessionPublisher(WeakSession session) : session_(std::move(session)) {}

  folly::coro::Task<TrackStatusResult> trackStatus(moxygen::TrackStatus req) override {
    moxygen::TrackStatusError closed{
        req.requestID,
        moxygen::TrackStatusErrorCode::INTERNAL_ERROR,
        std::string(kSessionClosed)
    };
    return withSession<TrackStatusResult>(
        session_,
        std::move(closed),
        [req = std::move(req)](moxygen::MoQSession& session) mutable {
          return session.trackStatus(std::move(req));
        }
    );
  }

  folly::coro::Task<SubscribeResult> subscribe(
      moxygen::SubscribeRequest sub,
      std::shared_ptr<moxygen::TrackConsumer> consumer
  ) override {
    moxygen::SubscribeError closed{
        sub.requestID,
        moxygen::SubscribeErrorCode::INTERNAL_ERROR,
        std::string(kSessionClosed)
    };
    return withSession<SubscribeResult>(
        session_,
        std::move(closed),
        [sub = std::move(sub), consumer = std::move(consumer)](moxygen::MoQSession& session
        ) mutable { return session.subscribe(std::move(sub), std::move(consumer)); }
    );
  }

  folly::coro::Task<FetchResult>
  fetch(moxygen::Fetch fetch, std::shared_ptr<moxygen::FetchConsumer> consumer) override {
    moxygen::FetchError closed{
        fetch.requestID,
        moxygen::FetchErrorCode::INTERNAL_ERROR,
        std::string(kSessionClosed)
    };
    return withSession<FetchResult>(
        session_,
        std::move(closed),
        [fetch = std::move(fetch), consumer = std::move(consumer)](moxygen::MoQSession& session
        ) mutable { return session.fetch(std::move(fetch), std::move(consumer)); }
    );
  }

  folly::coro::Task<SubscribeNamespaceResult> subscribeNamespace(
      moxygen::SubscribeNamespace subNs,
      std::shared_ptr<NamespacePublishHandle> handle
  ) override {
    moxygen::SubscribeNamespaceError closed{
        subNs.requestID,
        moxygen::SubscribeNamespaceErrorCode::INTERNAL_ERROR,
        std::string(kSessionClosed)
    };
    return withSession<SubscribeNamespaceResult>(
        session_,
        std::move(closed),
        [subNs = std::move(subNs), handle = std::move(handle)](moxygen::MoQSession& session
        ) mutable { return session.subscribeNamespace(std::move(subNs), std::move(handle)); }
    );
  }

  folly::coro::Task<SubscribeTracksResult> subscribeTracks(
      moxygen::SubscribeTracks subTracks,
      std::shared_ptr<PublishBlockedHandle> publishBlockedHandle
  ) override {
    moxygen::SubscribeTracksError closed{
        subTracks.requestID,
        moxygen::SubscribeTracksErrorCode::INTERNAL_ERROR,
        std::string(kSessionClosed)
    };
    return withSession<SubscribeTracksResult>(
        session_,
        std::move(closed),
        [subTracks = std::move(subTracks),
         publishBlockedHandle =
             std::move(publishBlockedHandle)](moxygen::MoQSession& session) mutable {
          return session.subscribeTracks(std::move(subTracks), std::move(publishBlockedHandle));
        }
    );
  }

private:
  WeakSession session_;
};

class WeakSessionSubscriber : public moxygen::Subscriber {
public:
  explicit WeakSessionSubscriber(WeakSession session) : session_(std::move(session)) {}

  folly::coro::Task<PublishNamespaceResult> publishNamespace(
      moxygen::PublishNamespace pubNs,
      std::shared_ptr<PublishNamespaceCallback> callback
  ) override {
    moxygen::PublishNamespaceError closed{
        pubNs.requestID,
        moxygen::PublishNamespaceErrorCode::INTERNAL_ERROR,
        std::string(kSessionClosed)
    };
    return withSession<PublishNamespaceResult>(
        session_,
        std::move(closed),
        [pubNs = std::move(pubNs), callback = std::move(callback)](moxygen::MoQSession& session
        ) mutable { return session.publishNamespace(std::move(pubNs), std::move(callback)); }
    );
  }

  PublishResult publish(
      moxygen::PublishRequest pub,
      std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle
  ) override {
    auto session = session_.lock();
    if (!session) {
      return folly::makeUnexpected(moxygen::PublishError{
          pub.requestID,
          moxygen::PublishErrorCode::INTERNAL_ERROR,
          std::string(kSessionClosed)
      });
    }
    return session->publish(std::move(pub), std::move(handle));
  }

private:
  WeakSession session_;
};

} // namespace

ParticipantInfo SessionParticipant::snapshot(const moxygen::MoQSession& session) {
  const bool relayHops = session.negotiatedSetupExtension(moxygen::SetupExtension::RelayHops);
  return ParticipantInfo{
      .sessionId = session.sessionId(),
      .version = session.getNegotiatedVersion().value_or(0),
      .executor = session.getExecutor(),
      .relayHops = relayHops,
      .peerHopID = session.getPeerHopID(),
      .linkCost = session.getRelayLinkCost(),
      .address = session.getPeerAddress().describe(),
      .authority = session.getAuthority(),
  };
}

Participant SessionParticipant::makeParticipant(
    const std::shared_ptr<moxygen::MoQSession>& session,
    folly::Executor* relayExec
) {
  std::shared_ptr<moxygen::Publisher> onExecutorPublisher =
      std::make_shared<WeakSessionPublisher>(WeakSession(session));
  std::shared_ptr<moxygen::Subscriber> onExecutorSubscriber =
      std::make_shared<WeakSessionSubscriber>(WeakSession(session));
  auto publisher = onExecutorPublisher;
  auto subscriber = onExecutorSubscriber;
  if (relayExec) {
    auto* exec = session->getExecutor();
    publisher = std::make_shared<PublisherCrossExecFilter>(exec, std::move(publisher));
    subscriber = std::make_shared<SubscriberCrossExecFilter>(exec, std::move(subscriber));
  }
  return Participant(
      std::make_shared<const ParticipantInfo>(snapshot(*session)),
      std::move(publisher),
      std::move(subscriber),
      std::move(onExecutorPublisher),
      std::move(onExecutorSubscriber)
  );
}

void SessionParticipant::attachToRequest() {
  if (!participant_) {
    auto session = getSession();
    XCHECK(session) << "a session request outlived its session";
    XDCHECK(onExecutor(session->getExecutor())) << "session request off its executor";
    participant_ = makeParticipant(session, relayExec_);
  }
  attachParticipant(*participant_);
}

folly::coro::Task<moxygen::Publisher::TrackStatusResult>
SessionParticipant::trackStatus(moxygen::TrackStatus req) {
  attachToRequest();
  return publisher_->trackStatus(std::move(req));
}

folly::coro::Task<moxygen::Publisher::SubscribeResult> SessionParticipant::subscribe(
    moxygen::SubscribeRequest sub,
    std::shared_ptr<moxygen::TrackConsumer> consumer
) {
  attachToRequest();
  // A copy: a SUBSCRIBE parked on a rendezvous can outlive this adapter.
  auto participant = *participant_;
  auto result = co_await publisher_->subscribe(std::move(sub), std::move(consumer));
  if (result.hasValue()) {
    result =
        std::make_shared<ParticipantSubscriptionHandle>(std::move(result.value()), participant);
  }
  co_return result;
}

folly::coro::Task<moxygen::Publisher::FetchResult>
SessionParticipant::fetch(moxygen::Fetch fetch, std::shared_ptr<moxygen::FetchConsumer> consumer) {
  attachToRequest();
  return publisher_->fetch(std::move(fetch), std::move(consumer));
}

folly::coro::Task<moxygen::Publisher::SubscribeNamespaceResult>
SessionParticipant::subscribeNamespace(
    moxygen::SubscribeNamespace subNs,
    std::shared_ptr<NamespacePublishHandle> handle
) {
  attachToRequest();
  return publisher_->subscribeNamespace(std::move(subNs), std::move(handle));
}

folly::coro::Task<moxygen::Publisher::SubscribeTracksResult> SessionParticipant::subscribeTracks(
    moxygen::SubscribeTracks subTracks,
    std::shared_ptr<PublishBlockedHandle> publishBlockedHandle
) {
  attachToRequest();
  return publisher_->subscribeTracks(std::move(subTracks), std::move(publishBlockedHandle));
}

folly::coro::Task<moxygen::Subscriber::PublishNamespaceResult> SessionParticipant::publishNamespace(
    moxygen::PublishNamespace pubNs,
    std::shared_ptr<PublishNamespaceCallback> callback
) {
  attachToRequest();
  return subscriber_->publishNamespace(std::move(pubNs), std::move(callback));
}

moxygen::Subscriber::PublishResult SessionParticipant::publish(
    moxygen::PublishRequest pub,
    std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle
) {
  attachToRequest();
  return subscriber_->publish(std::move(pub), std::move(handle));
}

void SessionParticipant::goaway(moxygen::Goaway goaway) {
  // The session calls this once as its publish handler and again as its subscribe handler.
  if (std::exchange(goawayForwarded_, true)) {
    return;
  }
  publisher_->goaway(goaway);
  subscriber_->goaway(std::move(goaway));
}

} // namespace openmoq::moqx
