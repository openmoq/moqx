/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// Per-session request gates that run the service's authorize chain before a
// request reaches the relay.
// - Installed only when the chain has an authorize hook.
// - A chain that completes inline costs no executor hop and keeps PUBLISH on
//   the relay's own path.

#include "plugin/Host.h"

#include <moxygen/MoQTypes.h>
#include <moxygen/Publisher.h>
#include <moxygen/Subscriber.h>

#include <folly/coro/Task.h>
#include <memory>

namespace openmoq::moqx {

moxygen::SessionCloseErrorCode toCloseCode(plugin::Error::Kind kind);
std::vector<moxygen::SetupParameter> toSetupParameters(const plugin::Params& params);
moxygen::RequestErrorCode toRequestCode(plugin::Error::Kind kind);

class PluginPublisherFilter : public moxygen::Publisher {
public:
  PluginPublisherFilter(
      std::shared_ptr<moxygen::Publisher> downstream,
      std::shared_ptr<plugin::PluginSession> session,
      bool peeringEnabled
  )
      : downstream_(std::move(downstream)), session_(std::move(session)),
        peeringEnabled_(peeringEnabled) {}

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
  std::shared_ptr<plugin::PluginSession> session_;
  bool peeringEnabled_;
};

class PluginSubscriberFilter : public moxygen::Subscriber {
public:
  PluginSubscriberFilter(
      std::shared_ptr<moxygen::Subscriber> downstream,
      std::shared_ptr<plugin::PluginSession> session,
      bool peeringEnabled
  )
      : downstream_(std::move(downstream)), session_(std::move(session)),
        peeringEnabled_(peeringEnabled) {}

  // A check that suspends answers with a deferred consumer
  // (consumerReady=false); the relay sees the PUBLISH once the check passes.
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
  std::shared_ptr<plugin::PluginSession> session_;
  bool peeringEnabled_;
};

} // namespace openmoq::moqx
