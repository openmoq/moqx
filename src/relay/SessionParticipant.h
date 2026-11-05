/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "relay/Participant.h"

#include <moxygen/MoQSession.h>
#include <moxygen/Publisher.h>
#include <moxygen/Subscriber.h>

#include <memory>
#include <optional>

namespace openmoq::moqx {

// The relay's publish and subscribe handler for one MoQ session. Attaches the session's Participant
// to each request.
class SessionParticipant : public moxygen::Publisher,
                           public moxygen::Subscriber,
                           public moxygen::SessionScoped {
public:
  SessionParticipant(
      std::shared_ptr<moxygen::Publisher> publisher,
      std::shared_ptr<moxygen::Subscriber> subscriber,
      folly::Executor* relayExec
  )
      : publisher_(std::move(publisher)), subscriber_(std::move(subscriber)),
        relayExec_(relayExec) {}

  // Must run on the session's executor, after SETUP.
  static ParticipantInfo snapshot(const moxygen::MoQSession& session);
  // With a relayExec, calls from the relay hop to the session's executor.
  static Participant
  makeParticipant(const std::shared_ptr<moxygen::MoQSession>& session, folly::Executor* relayExec);

  folly::coro::Task<TrackStatusResult> trackStatus(moxygen::TrackStatus req) override;

  folly::coro::Task<SubscribeResult> subscribe(
      moxygen::SubscribeRequest sub,
      std::shared_ptr<moxygen::TrackConsumer> consumer
  ) override;

  folly::coro::Task<FetchResult>
  fetch(moxygen::Fetch fetch, std::shared_ptr<moxygen::FetchConsumer> consumer) override;

  folly::coro::Task<SubscribeNamespaceResult> subscribeNamespace(
      moxygen::SubscribeNamespace subNs,
      std::shared_ptr<NamespacePublishHandle> handle
  ) override;

  folly::coro::Task<SubscribeTracksResult> subscribeTracks(
      moxygen::SubscribeTracks subTracks,
      std::shared_ptr<PublishBlockedHandle> publishBlockedHandle = nullptr
  ) override;

  folly::coro::Task<PublishNamespaceResult> publishNamespace(
      moxygen::PublishNamespace pubNs,
      std::shared_ptr<PublishNamespaceCallback> callback
  ) override;

  PublishResult publish(
      moxygen::PublishRequest pub,
      std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle
  ) override;

  void goaway(moxygen::Goaway goaway) override;

private:
  void attachToRequest();

  std::shared_ptr<moxygen::Publisher> publisher_;
  std::shared_ptr<moxygen::Subscriber> subscriber_;
  folly::Executor* relayExec_;
  // Built on the first request, after SETUP.
  std::optional<Participant> participant_;
  bool goawayForwarded_{false};
};

} // namespace openmoq::moqx
