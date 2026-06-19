/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "auth/Auth.h"

#include <moxygen/Publisher.h>
#include <moxygen/Subscriber.h>

#include <folly/coro/Task.h>
#include <memory>

namespace openmoq::moqx {

// Per-session publisher-side auth gate: authorizes each request against the
// session's grants, then forwards to the downstream publisher.
class AuthPublisherFilter : public moxygen::Publisher {
public:
  AuthPublisherFilter(
      std::shared_ptr<moxygen::Publisher> downstream,
      std::shared_ptr<const auth::AuthTokenVerifier> verifier,
      std::shared_ptr<const auth::Grants> grants,
      bool peeringEnabled
  )
      : downstream_(std::move(downstream)), verifier_(std::move(verifier)),
        grants_(std::move(grants)), peeringEnabled_(peeringEnabled) {}

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

  folly::coro::Task<TrackStatusResult> trackStatus(const moxygen::TrackStatus req) override;

  void goaway(moxygen::Goaway g) override;

private:
  std::shared_ptr<moxygen::Publisher> downstream_;
  std::shared_ptr<const auth::AuthTokenVerifier> verifier_;
  std::shared_ptr<const auth::Grants> grants_;
  bool peeringEnabled_;
};

// Per-session subscriber-side auth gate: authorizes PUBLISH/PUBLISH_NAMESPACE,
// then forwards to the downstream subscriber.
class AuthSubscriberFilter : public moxygen::Subscriber {
public:
  AuthSubscriberFilter(
      std::shared_ptr<moxygen::Subscriber> downstream,
      std::shared_ptr<const auth::AuthTokenVerifier> verifier,
      std::shared_ptr<const auth::Grants> grants
  )
      : downstream_(std::move(downstream)), verifier_(std::move(verifier)),
        grants_(std::move(grants)) {}

  PublishResult publish(
      moxygen::PublishRequest pub,
      std::shared_ptr<moxygen::SubscriptionHandle> handle
  ) override;

  folly::coro::Task<PublishNamespaceResult> publishNamespace(
      moxygen::PublishNamespace pubNs,
      std::shared_ptr<PublishNamespaceCallback> callback
  ) override;

  void goaway(moxygen::Goaway g) override;

private:
  std::shared_ptr<moxygen::Subscriber> downstream_;
  std::shared_ptr<const auth::AuthTokenVerifier> verifier_;
  std::shared_ptr<const auth::Grants> grants_;
};

} // namespace openmoq::moqx
