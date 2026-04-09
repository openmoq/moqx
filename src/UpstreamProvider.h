/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fizz/protocol/CertificateVerifier.h>
#include <folly/CancellationToken.h>
#include <folly/coro/SharedPromise.h>
#include <folly/coro/Sleep.h>
#include <folly/coro/Task.h>
#include <functional>
#include <moxygen/MoQClient.h>
#include <moxygen/MoQSession.h>
#include <moxygen/MoQTypes.h>
#include <moxygen/Publisher.h>
#include <moxygen/Subscriber.h>
#include <moxygen/events/MoQExecutor.h>
#include <optional>
#include <proxygen/lib/utils/URL.h>
#include <string>

namespace openmoq::moqx {

// --- Relay peering auth helpers ---
// Used by UpstreamProvider (initiating peer subNs) and MoqxRelay (reciprocation).

// If subNs carries a relay peer auth token, returns the relay ID encoded in
// the token (may be empty string if the peer didn't include one).
// Returns std::nullopt if subNs is not a peer subNs at all.
std::optional<std::string> getPeerRelayID(const moxygen::SubscribeNamespace& subNs);

// Builds a wildcard SubscribeNamespace(prefix={}, BOTH).
// With relayID: includes the relay auth token (initiating peer).
// Without relayID: no token (reciprocal response — prevents loops).
moxygen::SubscribeNamespace makePeerSubNs(std::optional<std::string> relayID = std::nullopt);

/**
 * UpstreamProvider connects to a remote MoQ endpoint as a client and presents
 * itself locally as both a Publisher and Subscriber. After connecting it issues
 * a wildcard subscribeNamespace(*, BOTH) with a relay auth token to initiate
 * the relay peering handshake. On disconnect it proactively reconnects
 * immediately, then backs off exponentially (1s → 60s cap) on repeated
 * failures.
 */
class UpstreamProvider : public moxygen::Publisher,
                         public moxygen::Subscriber,
                         public moxygen::MoQSession::MoQSessionCloseCallback,
                         public std::enable_shared_from_this<UpstreamProvider> {
public:
  using OnConnectHook =
      std::function<folly::coro::Task<void>(std::shared_ptr<moxygen::MoQSession>)>;
  using OnDisconnectHook = std::function<void()>;

  UpstreamProvider(
      std::shared_ptr<moxygen::MoQExecutor> exec,
      proxygen::URL url,
      std::shared_ptr<moxygen::Publisher> publishHandler,
      std::shared_ptr<moxygen::Subscriber> subscribeHandler,
      std::shared_ptr<fizz::CertificateVerifier> verifier = nullptr,
      OnConnectHook onConnect = nullptr,
      OnDisconnectHook onDisconnect = nullptr,
      std::chrono::milliseconds connectTimeout = std::chrono::milliseconds(5000),
      std::chrono::milliseconds idleTimeout = std::chrono::milliseconds(5000)
  );

  ~UpstreamProvider() override;

  // Initiates the first connection to the upstream server.
  folly::coro::Task<void> start();

  // Gracefully shuts down, closes the session, cancels reconnection.
  void stop();

  // Suspends until the upstream session is Connected (i.e., QUIC session
  // established + peering handshake completed), or the timeout elapses.
  // Returns immediately if already connected, stopped, or no connection is
  // in progress.  Swallows connection errors — caller must re-check state.
  folly::coro::Task<void> waitForConnected(std::chrono::milliseconds timeout);

  // --- Publisher interface (forwarded to upstream session) ---

  folly::coro::Task<SubscribeResult> subscribe(
      moxygen::SubscribeRequest sub,
      std::shared_ptr<moxygen::TrackConsumer> callback
  ) override;

  folly::coro::Task<FetchResult>
  fetch(moxygen::Fetch fetch, std::shared_ptr<moxygen::FetchConsumer> fetchCallback) override;

  folly::coro::Task<TrackStatusResult> trackStatus(moxygen::TrackStatus req) override;

  folly::coro::Task<SubscribeNamespaceResult> subscribeNamespace(
      moxygen::SubscribeNamespace subNs,
      std::shared_ptr<NamespacePublishHandle> handle
  ) override;

  // --- Subscriber interface (forwarded to upstream session) ---

  folly::coro::Task<PublishNamespaceResult> publishNamespace(
      moxygen::PublishNamespace ann,
      std::shared_ptr<PublishNamespaceCallback> cb = nullptr
  ) override;

  PublishResult publish(
      moxygen::PublishRequest pub,
      std::shared_ptr<moxygen::SubscriptionHandle> handle = nullptr
  ) override;

  // --- Goaway (shared by Publisher and Subscriber) ---
  void goaway(moxygen::Goaway goaway) override;

  // --- MoQSessionCloseCallback ---
  void onMoQSessionClosed(moxygen::SessionCloseErrorCode error, folly::Optional<uint32_t> wtError)
      override;

  // Access the current session (may be null)
  std::shared_ptr<moxygen::MoQSession> currentSession() const { return session_; }

  std::string stateString() const {
    switch (state_) {
    case State::Disconnected:
      return "disconnected";
    case State::Connecting:
      return "connecting";
    case State::Connected:
      return "connected";
    }
    return "unknown";
  }

private:
  enum class State { Disconnected, Connecting, Connected };

  // Returns the current session if already connected, null otherwise.
  // Used for the synchronous fast path in forwarding methods.
  std::shared_ptr<moxygen::MoQSession> getSession() const {
    if (!stopped_ && state_ == State::Connected && session_) {
      return session_;
    }
    return nullptr;
  }

  // Resets session_ and client_ on the exec thread (while EVBs are alive),
  // allowing ~MoQClientBase() to call moqSession_->close() on a live EVB.
  folly::coro::Task<void> close();

  // Ensures a session exists, connecting if needed. Called by slow-path
  // coroutines when getSession() returns null.
  folly::coro::Task<std::shared_ptr<moxygen::MoQSession>> getOrConnectSession();

  // Slow-path coroutine forwarders: called when not yet connected.
  folly::coro::Task<SubscribeResult>
  coSubscribe(moxygen::SubscribeRequest sub, std::shared_ptr<moxygen::TrackConsumer> callback);
  folly::coro::Task<FetchResult>
  coFetch(moxygen::Fetch fetch, std::shared_ptr<moxygen::FetchConsumer> fetchCallback);
  folly::coro::Task<TrackStatusResult> coTrackStatus(moxygen::TrackStatus req);
  folly::coro::Task<SubscribeNamespaceResult> coSubscribeNamespace(
      moxygen::SubscribeNamespace subNs,
      std::shared_ptr<NamespacePublishHandle> handle
  );
  folly::coro::Task<PublishNamespaceResult>
  coPublishNamespace(moxygen::PublishNamespace pubNs, std::shared_ptr<PublishNamespaceCallback> cb);
  folly::coro::Task<folly::Expected<moxygen::PublishOk, moxygen::PublishError>> coPublish(
      moxygen::PublishRequest pub,
      std::shared_ptr<moxygen::SubscriptionHandle> handle,
      std::shared_ptr<moxygen::TrackConsumer> pending,
      moxygen::RequestID reqID
  );

  // Performs the actual connection to upstream.
  folly::coro::Task<void> doConnect();

  // Resets the session state to Disconnected.
  void resetSession();

  // Tries to connect, retrying with exponential backoff until success or stop().
  // Exits once connected; onMoQSessionClosed()/goaway() spawn it again on drop.
  folly::coro::Task<void> reconnectLoop();

  State state_{State::Disconnected};
  std::unique_ptr<moxygen::MoQClient> client_;
  std::shared_ptr<moxygen::MoQSession> session_;
  std::shared_ptr<moxygen::Publisher> publishHandler_;
  std::shared_ptr<moxygen::Subscriber> subscribeHandler_;
  proxygen::URL url_;
  std::shared_ptr<moxygen::MoQExecutor> exec_;
  std::shared_ptr<fizz::CertificateVerifier> verifier_;
  OnConnectHook onConnect_;
  OnDisconnectHook onDisconnect_;
  std::chrono::milliseconds connectTimeout_;
  std::chrono::milliseconds idleTimeout_;
  bool stopped_{false};

  // Connection gating: when Connecting, operations co_await this
  std::optional<folly::coro::SharedPromise<folly::Unit>> connectPromise_;

  // Cancelled by stop() to break the reconnect loop out of backoff sleeps.
  folly::CancellationSource stopSource_;

  // Current backoff before next connect attempt. Reset to 0 on success,
  // doubled on failure (capped at kMaxReconnectBackoff).
  std::chrono::milliseconds reconnectBackoff_{0};
};

} // namespace openmoq::moqx
