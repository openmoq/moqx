/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "UpstreamProvider.h"
#include "relay/PublisherCrossExecFilter.h"
#include "relay/SubscriberCrossExecFilter.h"
#include <folly/coro/Timeout.h>
#include <moxygen/MoQFilters.h>
#include <moxygen/MoQRelaySession.h>
#include <moxygen/MoQVersions.h>

using namespace moxygen;

namespace openmoq::moqx {

// Randomly chosen token type identifying a relay-to-relay peering subNs.
// Must fit in a QUIC variable-length integer (top 2 bits must be 00, i.e. < 2^62).
static constexpr uint64_t kRelayAuthTokenType = 0x1B2C'3D4E'5F6A'7B8CULL;

std::optional<std::string> getPeerRelayID(const SubscribeNamespace& subNs) {
  const uint64_t authKey = static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN);
  for (const auto& param : subNs.params) {
    if (param.key == authKey && param.asAuthToken.tokenType == kRelayAuthTokenType) {
      return param.asAuthToken.tokenValue;
    }
  }
  return std::nullopt;
}

SubscribeNamespace makePeerSubNs(std::optional<std::string> relayID) {
  SubscribeNamespace subNs;
  subNs.trackNamespacePrefix = {};
  subNs.options = SubscribeNamespaceOptions::BOTH;
  subNs.forward = false;
  if (relayID) {
    subNs.params.insertParam(Parameter(
        static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN),
        AuthToken{
            .tokenType = kRelayAuthTokenType,
            .tokenValue = *relayID,
            .alias = AuthToken::DontRegister,
        }
    ));
  }
  return subNs;
}

namespace {

// TrackConsumer proxy whose downstream is set after connecting to upstream.
// Per MoQ protocol, publishers must not send data before PublishOk, so
// setDownstream() is always called before any forwarding methods are used.
class PendingTrackConsumer : public TrackConsumerFilter {
public:
  PendingTrackConsumer() : TrackConsumerFilter(nullptr) {}
};

// Presents an upstream SUBSCRIBE_NAMESPACE(options=PUBLISH) handle as a
// SubscribeTracksHandle, since subscribeTracks() maps onto subscribeNamespace().
class UpstreamSubscribeTracksHandle : public Publisher::SubscribeTracksHandle {
public:
  explicit UpstreamSubscribeTracksHandle(std::shared_ptr<Publisher::SubscribeNamespaceHandle> inner)
      : Publisher::SubscribeTracksHandle(inner->subscribeNamespaceOk()), inner_(std::move(inner)) {}

  void unsubscribeTracks() override { inner_->unsubscribeNamespace(); }

  folly::coro::Task<RequestUpdateResult> requestUpdate(RequestUpdate update) override {
    return inner_->requestUpdate(std::move(update));
  }

private:
  std::shared_ptr<Publisher::SubscribeNamespaceHandle> inner_;
};

} // namespace

// These adapters are only invoked by the cross-executor filters on exec_.
// Keeping session resolution here prevents session objects and provider state
// from escaping to the caller executor.
class UpstreamProvider::OwnerPublisher final : public Publisher {
public:
  explicit OwnerPublisher(std::shared_ptr<UpstreamProvider> provider)
      : provider_(std::move(provider)) {}

  folly::coro::Task<TrackStatusResult> trackStatus(TrackStatus req) override {
    return provider_->coTrackStatus(std::move(req));
  }

  folly::coro::Task<SubscribeResult>
  subscribe(SubscribeRequest sub, std::shared_ptr<TrackConsumer> callback) override {
    return provider_->coSubscribe(std::move(sub), std::move(callback));
  }

  folly::coro::Task<FetchResult>
  fetch(Fetch fetchReq, std::shared_ptr<FetchConsumer> callback) override {
    return provider_->coFetch(std::move(fetchReq), std::move(callback));
  }

  folly::coro::Task<SubscribeNamespaceResult> subscribeNamespace(
      SubscribeNamespace subNs,
      std::shared_ptr<NamespacePublishHandle> handle
  ) override {
    return provider_->coSubscribeNamespace(std::move(subNs), std::move(handle));
  }

private:
  std::shared_ptr<UpstreamProvider> provider_;
};

class UpstreamProvider::OwnerSubscriber final : public Subscriber {
public:
  explicit OwnerSubscriber(std::shared_ptr<UpstreamProvider> provider)
      : provider_(std::move(provider)) {}

  folly::coro::Task<PublishNamespaceResult> publishNamespace(
      PublishNamespace pubNs,
      std::shared_ptr<PublishNamespaceCallback> callback
  ) override {
    return provider_->coPublishNamespace(std::move(pubNs), std::move(callback));
  }

  PublishResult publish(PublishRequest pub, std::shared_ptr<SubscriptionHandle> handle) override {
    return provider_->publishOnOwner(std::move(pub), std::move(handle));
  }

private:
  std::shared_ptr<UpstreamProvider> provider_;
};

UpstreamProvider::UpstreamProvider(
    std::shared_ptr<MoQExecutor> exec,
    proxygen::URL url,
    std::shared_ptr<Publisher> publishHandler,
    std::shared_ptr<Subscriber> subscribeHandler,
    std::shared_ptr<fizz::CertificateVerifier> verifier,
    OnConnectHook onConnect,
    OnDisconnectHook onDisconnect,
    std::chrono::milliseconds connectTimeout,
    std::chrono::milliseconds idleTimeout,
    std::optional<uint64_t> clusterHopID,
    std::optional<uint64_t> relayCost
)
    : clusterHopID_(clusterHopID), relayCost_(relayCost),
      publishHandler_(std::move(publishHandler)), subscribeHandler_(std::move(subscribeHandler)),
      url_(std::move(url)), exec_(std::move(exec)), verifier_(std::move(verifier)),
      onConnect_(std::move(onConnect)), onDisconnect_(std::move(onDisconnect)),
      connectTimeout_(connectTimeout), idleTimeout_(idleTimeout) {
  XLOG(DBG1) << "UpstreamProvider created, url=" << url_.getUrl();
}

UpstreamProvider::~UpstreamProvider() {
  // stop() keeps this object alive through its owner-executor shutdown callback.
  // These must be null before member destructors run.
  XCHECK(!session_) << "UpstreamProvider dtor with live session; was stop() called?";
  XCHECK(!client_) << "UpstreamProvider dtor with live client; was stop() called?";
  XLOG(DBG1) << "UpstreamProvider destroyed";
}

folly::coro::Task<void> UpstreamProvider::start() {
  XLOG(DBG1) << "UpstreamProvider::start";
  auto self = shared_from_this();
  co_await folly::coro::co_withExecutor(
      folly::getKeepAliveToken(exec_.get()),
      self->reconnectLoop()
  );
}

static constexpr auto kInitialReconnectBackoff = std::chrono::seconds(1);
static constexpr auto kMaxReconnectBackoff = std::chrono::seconds(60);

folly::coro::Task<void> UpstreamProvider::reconnectLoop() {
  auto self = shared_from_this();
  while (!stopRequested_.load(std::memory_order_acquire) && !stopped_) {
    if (reconnectBackoff_.count() > 0) {
      XLOG(INFO) << "UpstreamProvider: reconnecting in " << reconnectBackoff_.count() << "ms";
      try {
        co_await folly::coro::co_withCancellation(
            stopSource_.getToken(),
            folly::coro::sleep(reconnectBackoff_)
        );
      } catch (const folly::OperationCancelled&) {
        co_return;
      }
    }
    if (stopRequested_.load(std::memory_order_acquire) || stopped_) {
      co_return;
    }

    try {
      co_await folly::coro::co_withCancellation(stopSource_.getToken(), getOrConnectSession());
      XLOG(DBG1) << "UpstreamProvider::reconnectLoop connected, session=" << session_.get();
      reconnectBackoff_ = std::chrono::milliseconds(0);
      co_return; // Connected — exit. onMoQSessionClosed()/goaway() will respawn.
    } catch (const folly::OperationCancelled&) {
      co_return;
    } catch (const std::exception& ex) {
      if (stopRequested_.load(std::memory_order_acquire) || stopped_) {
        co_return;
      }
      reconnectBackoff_ =
          reconnectBackoff_.count() == 0
              ? kInitialReconnectBackoff
              : std::min(reconnectBackoff_ * 2, std::chrono::milliseconds(kMaxReconnectBackoff));
      XLOG(ERR) << "UpstreamProvider: connect failed: " << ex.what() << ", retrying in "
                << reconnectBackoff_.count() << "ms";
    }
  }
}

void UpstreamProvider::close() {
  XLOG(DBG1) << "UpstreamProvider::close";
  session_.reset();
  client_.reset(); // ~MoQClientBase() calls moqSession_->close() implicitly
  state_ = State::Disconnected;
}

void UpstreamProvider::stop() {
  XLOG(DBG1) << "UpstreamProvider::stop";
  if (stopRequested_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  exec_->add([self = shared_from_this()]() { self->stopOnOwner(); });
}

void UpstreamProvider::stopOnOwner() {
  stopped_ = true;

  // Cancel any in-progress backoff sleep or connect in reconnectLoop().
  stopSource_.requestCancellation();

  // Break the shared_ptr cycle: relay owns us via upstream_, and we hold
  // back-refs to relay through these members.
  publishHandler_.reset();
  subscribeHandler_.reset();
  onConnect_ = nullptr;
  onDisconnect_ = nullptr;

  close();
}

// --- Publisher interface ---

folly::coro::Task<Publisher::SubscribeResult>
UpstreamProvider::subscribe(SubscribeRequest sub, std::shared_ptr<TrackConsumer> callback) {
  XLOG(DBG1) << "UpstreamProvider::subscribe ftn=" << sub.fullTrackName;
  auto forwarding = std::make_shared<PublisherCrossExecFilter>(
      exec_.get(),
      std::make_shared<OwnerPublisher>(shared_from_this())
  );
  co_return co_await forwarding->subscribe(std::move(sub), std::move(callback));
}

folly::coro::Task<Publisher::SubscribeResult>
UpstreamProvider::coSubscribe(SubscribeRequest sub, std::shared_ptr<TrackConsumer> callback) {
  auto sess = co_await getOrConnectSession();
  co_return co_await sess->subscribe(std::move(sub), std::move(callback));
}

folly::coro::Task<Publisher::FetchResult>
UpstreamProvider::fetch(Fetch fetch, std::shared_ptr<FetchConsumer> fetchCallback) {
  XLOG(DBG1) << "UpstreamProvider::fetch ftn=" << fetch.fullTrackName;
  auto forwarding = std::make_shared<PublisherCrossExecFilter>(
      exec_.get(),
      std::make_shared<OwnerPublisher>(shared_from_this())
  );
  co_return co_await forwarding->fetch(std::move(fetch), std::move(fetchCallback));
}

folly::coro::Task<Publisher::FetchResult>
UpstreamProvider::coFetch(Fetch fetch, std::shared_ptr<FetchConsumer> fetchCallback) {
  auto sess = co_await getOrConnectSession();
  co_return co_await sess->fetch(std::move(fetch), std::move(fetchCallback));
}

folly::coro::Task<Publisher::TrackStatusResult> UpstreamProvider::trackStatus(TrackStatus req) {
  XLOG(DBG1) << "UpstreamProvider::trackStatus ftn=" << req.fullTrackName;
  auto forwarding = std::make_shared<PublisherCrossExecFilter>(
      exec_.get(),
      std::make_shared<OwnerPublisher>(shared_from_this())
  );
  co_return co_await forwarding->trackStatus(std::move(req));
}

folly::coro::Task<Publisher::TrackStatusResult> UpstreamProvider::coTrackStatus(TrackStatus req) {
  auto sess = co_await getOrConnectSession();
  co_return co_await sess->trackStatus(req);
}

folly::coro::Task<Publisher::SubscribeNamespaceResult> UpstreamProvider::subscribeNamespace(
    SubscribeNamespace subNs,
    std::shared_ptr<NamespacePublishHandle> handle
) {
  XLOG(DBG1) << "UpstreamProvider::subscribeNamespace nsp=" << subNs.trackNamespacePrefix;
  auto forwarding = std::make_shared<PublisherCrossExecFilter>(
      exec_.get(),
      std::make_shared<OwnerPublisher>(shared_from_this())
  );
  co_return co_await forwarding->subscribeNamespace(std::move(subNs), std::move(handle));
}

folly::coro::Task<Publisher::SubscribeNamespaceResult> UpstreamProvider::coSubscribeNamespace(
    SubscribeNamespace subNs,
    std::shared_ptr<NamespacePublishHandle> handle
) {
  auto sess = co_await getOrConnectSession();
  co_return co_await sess->subscribeNamespace(std::move(subNs), std::move(handle));
}

folly::coro::Task<Publisher::SubscribeTracksResult> UpstreamProvider::subscribeTracks(
    SubscribeTracks subTracks,
    std::shared_ptr<PublishBlockedHandle> /*publishBlockedHandle*/
) {
  XLOG(DBG1) << "UpstreamProvider::subscribeTracks nsp=" << subTracks.trackNamespacePrefix;
  // SUBSCRIBE_TRACKS == SUBSCRIBE_NAMESPACE with options=PUBLISH: both request
  // PUBLISH for matching tracks under a prefix and want no NAMESPACE messages,
  // so there is no namespace handle to forward.
  SubscribeNamespace subNs;
  subNs.requestID = subTracks.requestID;
  subNs.trackNamespacePrefix = subTracks.trackNamespacePrefix;
  subNs.forward = subTracks.forward;
  subNs.params = std::move(subTracks.params);
  subNs.options = SubscribeNamespaceOptions::PUBLISH;
  auto result = co_await subscribeNamespace(std::move(subNs), /*handle=*/nullptr);
  if (result.hasError()) {
    co_return folly::makeUnexpected(std::move(result.error()));
  }
  co_return std::make_shared<UpstreamSubscribeTracksHandle>(std::move(result.value()));
}

// --- Subscriber interface ---

folly::coro::Task<Subscriber::PublishNamespaceResult> UpstreamProvider::publishNamespace(
    PublishNamespace pubNs,
    std::shared_ptr<PublishNamespaceCallback> cb
) {
  XLOG(DBG1) << "UpstreamProvider::publishNamespace ns=" << pubNs.trackNamespace;
  auto forwarding = std::make_shared<SubscriberCrossExecFilter>(
      exec_.get(),
      std::make_shared<OwnerSubscriber>(shared_from_this())
  );
  co_return co_await forwarding->publishNamespace(std::move(pubNs), std::move(cb));
}

folly::coro::Task<Subscriber::PublishNamespaceResult> UpstreamProvider::coPublishNamespace(
    PublishNamespace pubNs,
    std::shared_ptr<PublishNamespaceCallback> cb
) {
  auto sess = co_await getOrConnectSession();
  co_return co_await sess->publishNamespace(std::move(pubNs), std::move(cb));
}

Subscriber::PublishResult
UpstreamProvider::publish(PublishRequest pub, std::shared_ptr<moxygen::SubscriptionHandle> handle) {
  XLOG(DBG1) << "UpstreamProvider::publish ftn=" << pub.fullTrackName;
  if (stopRequested_.load(std::memory_order_acquire)) {
    return folly::makeUnexpected(
        PublishError{pub.requestID, PublishErrorCode::INTERNAL_ERROR, "UpstreamProvider stopped"}
    );
  }
  SubscriberCrossExecFilter forwarding(
      exec_.get(),
      std::make_shared<OwnerSubscriber>(shared_from_this())
  );
  return forwarding.publish(std::move(pub), std::move(handle));
}

Subscriber::PublishResult UpstreamProvider::publishOnOwner(
    PublishRequest pub,
    std::shared_ptr<moxygen::SubscriptionHandle> handle
) {
  if (stopRequested_.load(std::memory_order_acquire) || stopped_) {
    return folly::makeUnexpected(
        PublishError{pub.requestID, PublishErrorCode::INTERNAL_ERROR, "UpstreamProvider stopped"}
    );
  }
  if (auto session = getSession()) {
    return session->publish(std::move(pub), std::move(handle));
  }
  // Not connected — use a PendingTrackConsumer so the reply task can wire up
  // the real upstream consumer after connecting. Per MoQ protocol the
  // publisher must not send data before PublishOk, so setDownstream() is
  // guaranteed to be called before any forwarding methods.
  auto pending = std::make_shared<PendingTrackConsumer>();
  auto reqID = pub.requestID;
  auto reply = coPublish(std::move(pub), std::move(handle), pending, reqID);
  return Subscriber::PublishConsumerAndReplyTask{std::move(pending), std::move(reply)};
}

folly::coro::Task<folly::Expected<PublishOk, PublishError>> UpstreamProvider::coPublish(
    PublishRequest pub,
    std::shared_ptr<moxygen::SubscriptionHandle> handle,
    std::shared_ptr<TrackConsumer> pendingBase,
    RequestID reqID
) {
  try {
    auto session = co_await getOrConnectSession();
    auto result = session->publish(std::move(pub), std::move(handle));
    if (result.hasError()) {
      co_return folly::makeUnexpected(result.error());
    }
    std::static_pointer_cast<PendingTrackConsumer>(pendingBase)
        ->setDownstream(std::move(result.value().consumer));
    co_return co_await std::move(result.value().reply);
  } catch (const std::exception& ex) {
    co_return folly::makeUnexpected(PublishError{reqID, PublishErrorCode::INTERNAL_ERROR, ex.what()}
    );
  }
}

// --- Goaway ---

void UpstreamProvider::goaway(Goaway goaway) {
  XLOG(INFO) << "UpstreamProvider::goaway uri=" << goaway.newSessionUri;

  if (!goaway.newSessionUri.empty()) {
    XLOG(INFO) << "UpstreamProvider: updating URL from goaway: " << goaway.newSessionUri;
    url_ = proxygen::URL(goaway.newSessionUri);
  }

  const bool wasConnected = state_ == State::Connected;
  resetSession();
  if (wasConnected && !stopRequested_.load(std::memory_order_acquire) && !stopped_) {
    reconnectBackoff_ = std::chrono::milliseconds(0);
    co_withExecutor(exec_.get(), reconnectLoop()).start();
  }
}

// --- MoQSessionCloseCallback ---

void UpstreamProvider::onMoQSessionClosed(
    moxygen::SessionCloseErrorCode error,
    folly::Optional<uint32_t> wtError
) {
  XLOG(INFO) << "UpstreamProvider::onMoQSessionClosed error=" << (uint32_t)error
             << " wtError=" << (wtError ? *wtError : 0);
  const bool wasConnected = state_ == State::Connected;
  resetSession();
  if (wasConnected && !stopRequested_.load(std::memory_order_acquire) && !stopped_) {
    reconnectBackoff_ = std::chrono::milliseconds(0);
    co_withExecutor(exec_.get(), reconnectLoop()).start();
  }
}

// --- Private methods ---

folly::coro::Task<std::shared_ptr<MoQSession>> UpstreamProvider::getOrConnectSession() {
  if (stopRequested_.load(std::memory_order_acquire) || stopped_) {
    XLOG(DBG1) << "UpstreamProvider::getOrConnectSession - stopped";
    co_yield folly::coro::co_error(std::runtime_error("UpstreamProvider stopped"));
  }

  if (state_ == State::Connected && session_) {
    co_return session_;
  }

  if (state_ == State::Connecting) {
    XLOG(DBG1) << "UpstreamProvider::getOrConnectSession - waiting for "
                  "in-progress connection";
    CHECK(connectPromise_);
    co_await connectPromise_->getFuture();
    if (stopRequested_.load(std::memory_order_acquire) || !session_) {
      co_yield folly::coro::co_error(std::runtime_error("UpstreamProvider stopped"));
    }
    co_return session_;
  }

  // State::Disconnected - initiate connection
  XLOG(DBG1) << "UpstreamProvider::getOrConnectSession - initiating connection";
  state_ = State::Connecting;
  connectPromise_.emplace();

  try {
    co_await folly::coro::co_withCancellation(stopSource_.getToken(), doConnect());
    if (stopRequested_.load(std::memory_order_acquire)) {
      throw std::runtime_error("UpstreamProvider stopped");
    }
    state_ = State::Connected;
    XLOG(DBG1) << "UpstreamProvider: connected to upstream, session=" << session_.get();
    connectPromise_->setValue(folly::unit);
    co_return session_;
  } catch (const std::exception& ex) {
    XLOG(ERR) << "UpstreamProvider: connection failed: " << ex.what();
    state_ = State::Disconnected;
    client_.reset();
    connectPromise_->setException(folly::exception_wrapper(std::current_exception()));
    connectPromise_.reset();
    throw;
  }
}

folly::coro::Task<void> UpstreamProvider::doConnect() {
  XLOG(DBG1) << "UpstreamProvider::doConnect url=" << url_.getUrl();

  // Keep the client alive in this coroutine frame. stopOnOwner() may reset the
  // member while setup is suspended, but must not destroy an object whose
  // member coroutine is still active.
  auto client = std::make_shared<MoQClient>(
      exec_,
      url_,
      MoQRelaySession::createRelaySessionFactory(),
      verifier_
  );
  client_ = client;
  if (clusterHopID_) {
    client->addSetupParameter(SetupParameter(
        folly::to_underlying(SetupKey::RELAY_HOPS),
        encodeRelayHopID(*clusterHopID_, kVersionDraft18).value()
    ));
    if (relayCost_) {
      client->addSetupParameter(
          SetupParameter(folly::to_underlying(SetupKey::RELAY_COST), *relayCost_)
      );
    }
  }

  quic::TransportSettings ts;
  ts.orderedReadCallbacks = true;
  // Deep datagram queue dropping oldest-first: a write stall sheds stale frames
  // rather than rejecting new ones.
  ts.datagramConfig.writeBufSize = 1000;
  ts.datagramConfig.sendDropOldDataFirst = true;

  // Prefer cluster-capable draft 18, with draft 16 for ordinary relay chaining.
  co_await client->setupMoQSession(
      connectTimeout_,
      idleTimeout_,
      publishHandler_,
      subscribeHandler_,
      ts,
      getMoqtProtocols(clusterHopID_ ? "18,16" : "16", /*useStandard=*/true)
  );

  if (stopRequested_.load(std::memory_order_acquire) || client_ != client) {
    throw std::runtime_error("UpstreamProvider stopped");
  }
  auto session = client->moqSession_;
  CHECK(session) << "setupMoQSession succeeded but session is null";
  session_ = session;

  // Register for close notifications
  session->setSessionCloseCallback(this);

  // A coroutine lambda keeps its captures in its closure object. Retain a copy
  // in this coroutine frame so stopOnOwner() cannot destroy the closure while
  // its returned task is suspended.
  auto onConnect = onConnect_;
  if (onConnect) {
    co_await onConnect(session);
  }

  if (stopRequested_.load(std::memory_order_acquire) || client_ != client || session_ != session) {
    throw std::runtime_error("Upstream session closed during setup");
  }

  XLOG(DBG1) << "UpstreamProvider::doConnect completed, session=" << session_.get();
}

folly::coro::Task<void> UpstreamProvider::waitForConnected(std::chrono::milliseconds timeout) {
  auto self = shared_from_this();
  co_await folly::coro::co_withExecutor(
      folly::getKeepAliveToken(exec_.get()),
      self->waitForConnectedOnOwner(timeout)
  );
}

folly::coro::Task<void> UpstreamProvider::waitForConnectedOnOwner(std::chrono::milliseconds timeout
) {
  if (stopRequested_.load(std::memory_order_acquire) || stopped_ ||
      (state_ == State::Connected && session_)) {
    co_return;
  }
  if (!connectPromise_) {
    co_return;
  }
  co_await folly::coro::co_awaitTry(folly::coro::timeout(connectPromise_->getFuture(), timeout));
}

void UpstreamProvider::resetSession() {
  XLOG(DBG1) << "UpstreamProvider::resetSession";
  if (session_) {
    session_->setSessionCloseCallback(nullptr);
    if (onDisconnect_) {
      onDisconnect_();
    }
  }
  session_.reset();
  // Do NOT reset client_ here: ~MoQClientBase() calls moqSession_->close()
  // which would re-enter resetSession() while already inside a close() call
  // stack. client_ is replaced by doConnect() on the next connect, and torn
  // down cleanly by close() or the dtor safety net after EVBs die.
  state_ = State::Disconnected;
}

} // namespace openmoq::moqx
