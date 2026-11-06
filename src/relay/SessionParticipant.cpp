/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/SessionParticipant.h"

#include <folly/logging/xlog.h>

#include <utility>

namespace openmoq::moqx {

namespace {

// A REQUEST_UPDATE can carry a fill, which the relay serves as this participant.
class ParticipantSubscriptionHandle : public moxygen::SubscriptionHandle {
public:
  ParticipantSubscriptionHandle(
      std::shared_ptr<moxygen::SubscriptionHandle> inner,
      Participant participant
  )
      : inner_(std::move(inner)), participant_(std::move(participant)) {
    if (inner_->hasSubscribeOk()) {
      setSubscribeOk(inner_->subscribeOk());
    }
  }

  const moxygen::SubscribeOk& subscribeOk() const override { return inner_->subscribeOk(); }

  void unsubscribe() override { inner_->unsubscribe(); }

  void cancelFill(moxygen::RequestID requestID) override { inner_->cancelFill(requestID); }

  folly::coro::Task<RequestUpdateResult> requestUpdate(moxygen::RequestUpdate update) override {
    attachParticipant(participant_);
    return inner_->requestUpdate(std::move(update));
  }

private:
  std::shared_ptr<moxygen::SubscriptionHandle> inner_;
  Participant participant_;
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

Participant SessionParticipant::makeParticipant(const std::shared_ptr<moxygen::MoQSession>& session
) {
  return Participant{.info = std::make_shared<const ParticipantInfo>(snapshot(*session))};
}

void SessionParticipant::attachToRequest() {
  if (!participant_) {
    auto session = getSession();
    XCHECK(session) << "a session request outlived its session";
    participant_ = makeParticipant(session);
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
