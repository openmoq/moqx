/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <moqx/MoqxRelay.h>
#include <moqx/UpstreamProvider.h>
#include <moxygen/MoQFilters.h>
#include <moxygen/MoQRelaySession.h>
#include <moxygen/MoQVersions.h>

using namespace moxygen;

namespace openmoq::moqx {

// Randomly chosen token type identifying a relay-to-relay peering subNs.
// Must fit in a QUIC variable-length integer (top 2 bits must be 00, i.e. < 2^62).
static constexpr uint64_t kRelayAuthTokenType = 0x1B2C'3D4E'5F6A'7B8CULL;

bool isPeerSubNs(const SubscribeNamespace& subNs) {
  const uint64_t authKey = static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN);
  for (const auto& param : subNs.params) {
    if (param.key == authKey && param.asAuthToken.tokenType == kRelayAuthTokenType) {
      return true;
    }
  }
  return false;
}

SubscribeNamespace makePeerSubNs(std::optional<std::string> relayID) {
  SubscribeNamespace subNs;
  subNs.trackNamespacePrefix = {};
  subNs.options = SubscribeNamespaceOptions::BOTH;
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

// TrackConsumer proxy whose downstream is set after lazy connect completes.
// Per MoQ protocol, publishers must not send data before PublishOk, so
// setDownstream() is always called before any forwarding methods are used.
class PendingTrackConsumer : public TrackConsumerFilter {
public:
  PendingTrackConsumer() : TrackConsumerFilter(nullptr) {}

  void setDownstream(std::shared_ptr<TrackConsumer> downstream) {
    downstream_ = std::move(downstream);
  }
};

} // namespace

UpstreamProvider::UpstreamProvider(
    std::shared_ptr<MoQExecutor> exec,
    proxygen::URL url,
    std::shared_ptr<Publisher> publishHandler,
    std::shared_ptr<Subscriber> subscribeHandler,
    std::shared_ptr<fizz::CertificateVerifier> verifier,
    std::string relayID
)
    : publishHandler_(std::move(publishHandler)), subscribeHandler_(std::move(subscribeHandler)),
      url_(std::move(url)), exec_(std::move(exec)), verifier_(std::move(verifier)),
      relayID_(std::move(relayID)) {
  XLOG(DBG1) << "UpstreamProvider created, url=" << url_.getUrl();
}

UpstreamProvider::~UpstreamProvider() {
  XLOG(DBG1) << "UpstreamProvider destroyed";
}

folly::coro::Task<void> UpstreamProvider::start() {
  XLOG(DBG1) << "UpstreamProvider::start";
  co_await reconnectLoop();
}

static constexpr auto kInitialReconnectBackoff = std::chrono::seconds(1);
static constexpr auto kMaxReconnectBackoff = std::chrono::seconds(60);

folly::coro::Task<void> UpstreamProvider::reconnectLoop() {
  while (!stopped_) {
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
    if (stopped_) {
      co_return;
    }

    try {
      co_await getOrConnectSession();
      XLOG(DBG1) << "UpstreamProvider::reconnectLoop connected, session=" << session_.get();
      reconnectBackoff_ = std::chrono::milliseconds(0);
      co_return; // Connected — exit. onMoQSessionClosed()/goaway() will respawn.
    } catch (const std::exception& ex) {
      if (stopped_) {
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

void UpstreamProvider::stop() {
  XLOG(DBG1) << "UpstreamProvider::stop";
  stopped_ = true;

  // Cancel any in-progress backoff sleep in reconnectLoop().
  stopSource_.requestCancellation();

  if (session_) {
    session_->setSessionCloseCallback(nullptr);
    session_->close(SessionCloseErrorCode::NO_ERROR);
    session_.reset();
  }
  client_.reset();
  state_ = State::Disconnected;

  // Fail any waiters on the connect promise (only if not already fulfilled)
  if (connectPromise_ && !connectPromise_->isFulfilled()) {
    connectPromise_->setException(
        folly::exception_wrapper(std::runtime_error("UpstreamProvider stopped"))
    );
  }
  connectPromise_.reset();
}

// --- Publisher interface ---

folly::coro::Task<Publisher::SubscribeResult>
UpstreamProvider::subscribe(SubscribeRequest sub, std::shared_ptr<TrackConsumer> callback) {
  XLOG(DBG1) << "UpstreamProvider::subscribe ftn=" << sub.fullTrackName;
  if (auto sess = getSession()) {
    return sess->subscribe(std::move(sub), std::move(callback));
  }
  return coSubscribe(std::move(sub), std::move(callback));
}

folly::coro::Task<Publisher::SubscribeResult>
UpstreamProvider::coSubscribe(SubscribeRequest sub, std::shared_ptr<TrackConsumer> callback) {
  auto sess = co_await getOrConnectSession();
  co_return co_await sess->subscribe(std::move(sub), std::move(callback));
}

folly::coro::Task<Publisher::FetchResult>
UpstreamProvider::fetch(Fetch fetch, std::shared_ptr<FetchConsumer> fetchCallback) {
  XLOG(DBG1) << "UpstreamProvider::fetch ftn=" << fetch.fullTrackName;
  if (auto sess = getSession()) {
    return sess->fetch(std::move(fetch), std::move(fetchCallback));
  }
  return coFetch(std::move(fetch), std::move(fetchCallback));
}

folly::coro::Task<Publisher::FetchResult>
UpstreamProvider::coFetch(Fetch fetch, std::shared_ptr<FetchConsumer> fetchCallback) {
  auto sess = co_await getOrConnectSession();
  co_return co_await sess->fetch(std::move(fetch), std::move(fetchCallback));
}

folly::coro::Task<Publisher::TrackStatusResult> UpstreamProvider::trackStatus(TrackStatus req) {
  XLOG(DBG1) << "UpstreamProvider::trackStatus ftn=" << req.fullTrackName;
  if (auto sess = getSession()) {
    return sess->trackStatus(req);
  }
  return coTrackStatus(req);
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
  if (auto sess = getSession()) {
    return sess->subscribeNamespace(std::move(subNs), std::move(handle));
  }
  return coSubscribeNamespace(std::move(subNs), std::move(handle));
}

folly::coro::Task<Publisher::SubscribeNamespaceResult> UpstreamProvider::coSubscribeNamespace(
    SubscribeNamespace subNs,
    std::shared_ptr<NamespacePublishHandle> handle
) {
  auto sess = co_await getOrConnectSession();
  co_return co_await sess->subscribeNamespace(std::move(subNs), std::move(handle));
}

// --- Subscriber interface ---

folly::coro::Task<Subscriber::PublishNamespaceResult> UpstreamProvider::publishNamespace(
    PublishNamespace pubNs,
    std::shared_ptr<PublishNamespaceCallback> cb
) {
  XLOG(DBG1) << "UpstreamProvider::publishNamespace ns=" << pubNs.trackNamespace;
  if (auto sess = getSession()) {
    return sess->publishNamespace(std::move(pubNs), std::move(cb));
  }
  return coPublishNamespace(std::move(pubNs), std::move(cb));
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
  if (stopped_) {
    return folly::makeUnexpected(
        PublishError{pub.requestID, PublishErrorCode::INTERNAL_ERROR, "UpstreamProvider stopped"}
    );
  }
  if (state_ == State::Connected && session_) {
    return session_->publish(std::move(pub), std::move(handle));
  }
  // Not connected — use a PendingTrackConsumer so the reply task can wire up
  // the real upstream consumer after lazy connect. Per MoQ protocol the
  // publisher must not send data before PublishOk, so setDownstream() is
  // guaranteed to be called before any forwarding methods.
  auto pending = std::make_shared<PendingTrackConsumer>();
  auto reqID = pub.requestID;
  auto reply = [this, pub = std::move(pub), handle = std::move(handle), pending, reqID](
               ) mutable -> folly::coro::Task<folly::Expected<PublishOk, PublishError>> {
    try {
      auto session = co_await getOrConnectSession();
      auto result = session->publish(std::move(pub), std::move(handle));
      if (result.hasError()) {
        co_return folly::makeUnexpected(result.error());
      }
      pending->setDownstream(std::move(result.value().consumer));
      co_return co_await std::move(result.value().reply);
    } catch (const std::exception& ex) {
      co_return folly::makeUnexpected(
          PublishError{reqID, PublishErrorCode::INTERNAL_ERROR, ex.what()}
      );
    }
  }();
  return Subscriber::PublishConsumerAndReplyTask{std::move(pending), std::move(reply)};
}

// --- Goaway ---

void UpstreamProvider::goaway(Goaway goaway) {
  XLOG(INFO) << "UpstreamProvider::goaway uri=" << goaway.newSessionUri;

  if (!goaway.newSessionUri.empty()) {
    XLOG(INFO) << "UpstreamProvider: updating URL from goaway: " << goaway.newSessionUri;
    url_ = proxygen::URL(goaway.newSessionUri);
  }

  resetSession();
  if (!stopped_) {
    reconnectBackoff_ = kInitialReconnectBackoff;
    co_withExecutor(exec_.get(), reconnectLoop()).start();
  }
}

// --- MoQSessionCloseCallback ---

void UpstreamProvider::onMoQSessionClosed() {
  XLOG(INFO) << "UpstreamProvider::onMoQSessionClosed";
  resetSession();
  if (!stopped_) {
    reconnectBackoff_ = kInitialReconnectBackoff;
    co_withExecutor(exec_.get(), reconnectLoop()).start();
  }
}

// --- Private methods ---

folly::coro::Task<std::shared_ptr<MoQSession>> UpstreamProvider::getOrConnectSession() {
  if (stopped_) {
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
    if (!session_) {
      co_yield folly::coro::co_error(std::runtime_error("Connection failed"));
    }
    co_return session_;
  }

  // State::Disconnected - initiate connection
  XLOG(DBG1) << "UpstreamProvider::getOrConnectSession - initiating connection";
  state_ = State::Connecting;
  connectPromise_.emplace();

  try {
    co_await doConnect();
    state_ = State::Connected;
    XLOG(DBG1) << "UpstreamProvider: connected to upstream, session=" << session_.get();
    connectPromise_->setValue(folly::unit);
    co_return session_;
  } catch (const std::exception& ex) {
    XLOG(ERR) << "UpstreamProvider: connection failed: " << ex.what();
    state_ = State::Disconnected;
    connectPromise_->setException(folly::exception_wrapper(std::current_exception()));
    connectPromise_.reset();
    throw;
  }
}

folly::coro::Task<void> UpstreamProvider::doConnect() {
  XLOG(DBG1) << "UpstreamProvider::doConnect url=" << url_.getUrl();

  client_ = std::make_unique<MoQClient>(
      exec_,
      url_,
      MoQRelaySession::createRelaySessionFactory(),
      verifier_
  );

  quic::TransportSettings ts;
  ts.orderedReadCallbacks = true;

  // Relay chaining requires draft 16+. Only offer standard draft-16 ALPN
  // ("moqt-16") so we fail fast if the upstream doesn't support it.
  // TODO: make timeouts configurable via UpstreamConfig
  static constexpr auto kConnectTimeout = std::chrono::milliseconds(5000);
  static constexpr auto kTransactionTimeout = std::chrono::milliseconds(5000);
  co_await client_->setupMoQSession(
      kConnectTimeout,
      kTransactionTimeout,
      publishHandler_,
      subscribeHandler_,
      ts,
      getMoqtProtocols("16", /*useStandard=*/true)
  );

  session_ = client_->moqSession_;
  CHECK(session_) << "setupMoQSession succeeded but session is null";

  // Register for close notifications
  session_->setSessionCloseCallback(this);

  // Relay peering handshake: subscribe to all namespaces with relay auth token.
  // The upstream relay recognises the token and reciprocates, populating our
  // local namespace tree via the existing announcement/publish machinery.
  if (!relayID_.empty()) {
    XLOG(DBG1) << "UpstreamProvider: issuing peer subNs, relayID=" << relayID_;
    // Bridge NAMESPACE/NAMESPACE_DONE messages (draft 16+) back into the local
    // relay via subscribeHandler_. If subscribeHandler_ is absent, fall back to
    // a no-op handle (namespace announcements arrive via PUBLISH_NAMESPACE only).
    auto relay = std::dynamic_pointer_cast<MoqxRelay>(subscribeHandler_);
    auto nsHandle = makeNamespaceBridgeHandle(relay, session_);
    auto result = co_await session_->subscribeNamespace(makePeerSubNs(relayID_), nsHandle);
    if (result.hasValue()) {
      peerSubNsHandle_ = std::move(result.value());
    } else {
      XLOG(ERR) << "UpstreamProvider: peer subNs failed: " << result.error().reasonPhrase;
    }
  }

  XLOG(DBG1) << "UpstreamProvider::doConnect completed, session=" << session_.get();
}

void UpstreamProvider::resetSession() {
  XLOG(DBG1) << "UpstreamProvider::resetSession";
  peerSubNsHandle_.reset(); // drop before session so unsubscribe is not sent
  if (session_) {
    session_->setSessionCloseCallback(nullptr);
  }
  session_.reset();
  client_.reset();
  state_ = State::Disconnected;
}

} // namespace openmoq::moqx
