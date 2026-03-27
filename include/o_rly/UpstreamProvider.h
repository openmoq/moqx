/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fizz/protocol/CertificateVerifier.h>
#include <folly/CancellationToken.h>
#include <folly/coro/SharedPromise.h>
#include <folly/coro/Task.h>
#include <moxygen/MoQClient.h>
#include <moxygen/MoQSession.h>
#include <moxygen/Publisher.h>
#include <moxygen/Subscriber.h>
#include <moxygen/events/MoQExecutor.h>
#include <proxygen/lib/utils/URL.h>
#include <string>

namespace openmoq::o_rly {

/**
 * UpstreamProvider connects to a remote MoQ endpoint as a client and presents
 * itself locally as both a Publisher and Subscriber. When the relay receives
 * a subscribe from a downstream client, it can forward it through the
 * UpstreamProvider to the upstream server.
 *
 * Session lifecycle: if the session receives a GOAWAY or is disconnected,
 * reconnection is lazy - triggered by the next operation.
 */
class UpstreamProvider
    : public moxygen::Publisher,
      public moxygen::Subscriber,
      public moxygen::MoQSession::MoQSessionCloseCallback,
      public std::enable_shared_from_this<UpstreamProvider> {
 public:
  UpstreamProvider(
      std::shared_ptr<moxygen::MoQExecutor> exec,
      proxygen::URL url,
      std::shared_ptr<moxygen::Publisher> publishHandler,
      std::shared_ptr<moxygen::Subscriber> subscribeHandler,
      std::shared_ptr<fizz::CertificateVerifier> verifier = nullptr,
      std::string relayID = {});

  ~UpstreamProvider() override;

  // Initiates the first connection to the upstream server.
  folly::coro::Task<void> start();

  // Gracefully shuts down, closes the session, cancels reconnection.
  void stop();

  // --- Publisher interface (forwarded to upstream session) ---

  folly::coro::Task<SubscribeResult> subscribe(
      moxygen::SubscribeRequest sub,
      std::shared_ptr<moxygen::TrackConsumer> callback) override;

  folly::coro::Task<FetchResult> fetch(
      moxygen::Fetch fetch,
      std::shared_ptr<moxygen::FetchConsumer> fetchCallback) override;

  folly::coro::Task<TrackStatusResult> trackStatus(
      moxygen::TrackStatus req) override;

  folly::coro::Task<SubscribeNamespaceResult> subscribeNamespace(
      moxygen::SubscribeNamespace subNs,
      std::shared_ptr<NamespacePublishHandle> handle) override;

  // --- Subscriber interface (forwarded to upstream session) ---

  folly::coro::Task<PublishNamespaceResult> publishNamespace(
      moxygen::PublishNamespace ann,
      std::shared_ptr<PublishNamespaceCallback> cb = nullptr) override;

  PublishResult publish(
      moxygen::PublishRequest pub,
      std::shared_ptr<moxygen::SubscriptionHandle> handle = nullptr) override;

  // --- Goaway (shared by Publisher and Subscriber) ---
  void goaway(moxygen::Goaway goaway) override;

  // --- MoQSessionCloseCallback ---
  void onMoQSessionClosed() override;

  // Access the current session (may be null)
  std::shared_ptr<moxygen::MoQSession> currentSession() const {
    return session_;
  }

 private:
  enum class State { Disconnected, Connecting, Connected };

  // Ensures a session exists, connecting if needed. All forwarding methods
  // call this before accessing session_.
  folly::coro::Task<std::shared_ptr<moxygen::MoQSession>>
  getOrConnectSession();

  // Performs the actual connection to upstream.
  folly::coro::Task<void> doConnect();

  // Resets the session state to Disconnected.
  void resetSession();

  State state_{State::Disconnected};
  std::unique_ptr<moxygen::MoQClient> client_;
  std::shared_ptr<moxygen::MoQSession> session_;
  std::shared_ptr<moxygen::Publisher> publishHandler_;
  std::shared_ptr<moxygen::Subscriber> subscribeHandler_;
  proxygen::URL url_;
  std::shared_ptr<moxygen::MoQExecutor> exec_;
  std::shared_ptr<fizz::CertificateVerifier> verifier_;
  std::string relayID_;
  bool stopped_{false};

  // Connection gating: when Connecting, operations co_await this
  std::optional<folly::coro::SharedPromise<folly::Unit>> connectPromise_;
};

} // namespace openmoq::o_rly
