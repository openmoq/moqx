/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <moqx/UpstreamProvider.h>
#include <moxygen/MoQFilters.h>
#include <moxygen/MoQRelaySession.h>
#include <moxygen/MoQVersions.h>
#include <folly/logging/xlog.h>

using namespace moxygen;

namespace openmoq::moqx {

// Randomly chosen token type identifying a relay-to-relay peering subNs.
static constexpr uint64_t kRelayAuthTokenType = 0xA3F7'C291'5B84'E60DULL;

bool isPeerSubNs(const SubscribeNamespace& subNs) {
  const uint64_t authKey =
      static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN);
  for (const auto& param : subNs.params) {
    if (param.key == authKey &&
        param.asAuthToken.tokenType == kRelayAuthTokenType) {
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
        }));
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

// Bridges NAMESPACE / NAMESPACE_DONE messages (draft 16+) from a peer relay
// into the local relay's publishNamespace machinery. Used for both the
// initiating peer subNs (UpstreamProvider) and the reciprocal (ORelay).
// One instance is created per connect; recreated on reconnect so the handle
// map is always fresh (peer re-announces all namespaces after reconnect).
class NamespaceBridgeHandle
    : public Publisher::NamespacePublishHandle,
      public std::enable_shared_from_this<NamespaceBridgeHandle> {
 public:
  NamespaceBridgeHandle(
      std::weak_ptr<Subscriber> relay,
      std::shared_ptr<MoQSession> session)
      : relay_(std::move(relay)), session_(std::move(session)) {}

  void namespaceMsg(const TrackNamespace& suffix) override {
    auto relay = relay_.lock();
    if (!relay || !session_) {
      return;
    }
    auto self = shared_from_this();
    auto exec = session_->getExecutor();
    co_withExecutor(
        exec,
        [relay, suffix, self]() mutable -> folly::coro::Task<void> {
          PublishNamespace ann;
          ann.trackNamespace = suffix;
          auto result = co_await relay->publishNamespace(ann, nullptr);
          if (result.hasValue()) {
            self->storeHandle(suffix, std::move(result.value()));
          } else {
            XLOG(ERR) << "NamespaceBridgeHandle::namespaceMsg failed: "
                      << result.error().reasonPhrase;
          }
        }())
        .start();
  }

  void namespaceDoneMsg(const TrackNamespace& suffix) override {
    auto handle = removeHandle(suffix);
    if (handle) {
      handle->publishNamespaceDone();
    }
  }

 private:
  void storeHandle(
      const TrackNamespace& ns,
      std::shared_ptr<Subscriber::PublishNamespaceHandle> h) {
    handles_[ns] = std::move(h);
  }

  std::shared_ptr<Subscriber::PublishNamespaceHandle> removeHandle(
      const TrackNamespace& ns) {
    auto it = handles_.find(ns);
    if (it == handles_.end()) {
      return nullptr;
    }
    auto h = std::move(it->second);
    handles_.erase(it);
    return h;
  }

  std::weak_ptr<Subscriber> relay_;
  std::shared_ptr<MoQSession> session_;
  // Namespace → handle map; all callbacks run on the session's EVB so no lock.
  std::unordered_map<
      TrackNamespace,
      std::shared_ptr<Subscriber::PublishNamespaceHandle>,
      TrackNamespace::hash>
      handles_;
};

std::shared_ptr<Publisher::NamespacePublishHandle> makeNamespaceBridgeHandle(
    std::weak_ptr<Subscriber> relay,
    std::shared_ptr<MoQSession> session) {
  return std::make_shared<NamespaceBridgeHandle>(
      std::move(relay), std::move(session));
}

UpstreamProvider::UpstreamProvider(
    std::shared_ptr<MoQExecutor> exec,
    proxygen::URL url,
    std::shared_ptr<Publisher> publishHandler,
    std::shared_ptr<Subscriber> subscribeHandler,
    std::shared_ptr<fizz::CertificateVerifier> verifier,
    std::string relayID)
    : publishHandler_(std::move(publishHandler)),
      subscribeHandler_(std::move(subscribeHandler)),
      url_(std::move(url)),
      exec_(std::move(exec)),
      verifier_(std::move(verifier)),
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
      XLOG(INFO) << "UpstreamProvider: reconnecting in "
                 << reconnectBackoff_.count() << "ms";
      try {
        co_await folly::coro::co_withCancellation(
            stopSource_.getToken(), folly::coro::sleep(reconnectBackoff_));
      } catch (const folly::OperationCancelled&) {
        co_return;
      }
    }
    if (stopped_) {
      co_return;
    }

    try {
      co_await getOrConnectSession();
      XLOG(DBG1) << "UpstreamProvider::reconnectLoop connected, session="
                 << session_.get();
      reconnectBackoff_ = std::chrono::milliseconds(0);
      co_return; // Connected — exit. onMoQSessionClosed()/goaway() will respawn.
    } catch (const std::exception& ex) {
      if (stopped_) {
        co_return;
      }
      reconnectBackoff_ = reconnectBackoff_.count() == 0
          ? kInitialReconnectBackoff
          : std::min(reconnectBackoff_ * 2,
                     std::chrono::milliseconds(kMaxReconnectBackoff));
      XLOG(ERR) << "UpstreamProvider: connect failed: " << ex.what()
                << ", retrying in " << reconnectBackoff_.count() << "ms";
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
    connectPromise_->setException(folly::exception_wrapper(
        std::runtime_error("UpstreamProvider stopped")));
  }
  connectPromise_.reset();

}

// --- Publisher interface ---

folly::coro::Task<Publisher::SubscribeResult> UpstreamProvider::subscribe(
    SubscribeRequest sub,
    std::shared_ptr<TrackConsumer> callback) {
  XLOG(DBG1) << "UpstreamProvider::subscribe ftn=" << sub.fullTrackName;
  auto session = co_await getOrConnectSession();
  co_return co_await session->subscribe(std::move(sub), std::move(callback));
}

folly::coro::Task<Publisher::FetchResult> UpstreamProvider::fetch(
    Fetch fetch,
    std::shared_ptr<FetchConsumer> fetchCallback) {
  XLOG(DBG1) << "UpstreamProvider::fetch ftn=" << fetch.fullTrackName;
  auto session = co_await getOrConnectSession();
  co_return co_await session->fetch(std::move(fetch), std::move(fetchCallback));
}

folly::coro::Task<Publisher::TrackStatusResult> UpstreamProvider::trackStatus(
    TrackStatus req) {
  XLOG(DBG1) << "UpstreamProvider::trackStatus ftn=" << req.fullTrackName;
  auto session = co_await getOrConnectSession();
  co_return co_await session->trackStatus(req);
}

folly::coro::Task<Publisher::SubscribeNamespaceResult>
UpstreamProvider::subscribeNamespace(
    SubscribeNamespace subNs,
    std::shared_ptr<NamespacePublishHandle> handle) {
  XLOG(DBG1) << "UpstreamProvider::subscribeNamespace nsp="
             << subNs.trackNamespacePrefix;
  auto session = co_await getOrConnectSession();
  co_return co_await session->subscribeNamespace(
      std::move(subNs), std::move(handle));
}

// --- Subscriber interface ---

folly::coro::Task<Subscriber::PublishNamespaceResult>
UpstreamProvider::publishNamespace(
    PublishNamespace ann,
    std::shared_ptr<PublishNamespaceCallback> cb) {
  XLOG(DBG1) << "UpstreamProvider::publishNamespace ns="
             << ann.trackNamespace;
  auto session = co_await getOrConnectSession();
  co_return co_await session->publishNamespace(std::move(ann), std::move(cb));
}

Subscriber::PublishResult UpstreamProvider::publish(
    PublishRequest pub,
    std::shared_ptr<moxygen::SubscriptionHandle> handle) {
  XLOG(DBG1) << "UpstreamProvider::publish ftn=" << pub.fullTrackName;
  if (stopped_) {
    return folly::makeUnexpected(
        PublishError{pub.requestID, PublishErrorCode::INTERNAL_ERROR,
                     "UpstreamProvider stopped"});
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
  auto reply =
      [this,
       pub = std::move(pub),
       handle = std::move(handle),
       pending,
       reqID]() mutable
      -> folly::coro::Task<folly::Expected<PublishOk, PublishError>> {
    try {
      auto session = co_await getOrConnectSession();
      auto result = session->publish(std::move(pub), std::move(handle));
      if (result.hasError()) {
        co_return folly::makeUnexpected(result.error());
      }
      pending->setDownstream(std::move(result.value().consumer));
      co_return co_await std::move(result.value().reply);
    } catch (const std::exception& ex) {
      co_return folly::makeUnexpected(PublishError{
          reqID, PublishErrorCode::INTERNAL_ERROR, ex.what()});
    }
  }();
  return Subscriber::PublishConsumerAndReplyTask{
      std::move(pending), std::move(reply)};
}

// --- Goaway ---

void UpstreamProvider::goaway(Goaway goaway) {
  XLOG(INFO) << "UpstreamProvider::goaway uri=" << goaway.newSessionUri;

  if (!goaway.newSessionUri.empty()) {
    XLOG(INFO) << "UpstreamProvider: updating URL from goaway: "
               << goaway.newSessionUri;
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

folly::coro::Task<std::shared_ptr<MoQSession>>
UpstreamProvider::getOrConnectSession() {
  if (stopped_) {
    XLOG(DBG1) << "UpstreamProvider::getOrConnectSession - stopped";
    co_yield folly::coro::co_error(
        std::runtime_error("UpstreamProvider stopped"));
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
      co_yield folly::coro::co_error(
          std::runtime_error("Connection failed"));
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
    XLOG(DBG1) << "UpstreamProvider: connected to upstream, session="
               << session_.get();
    connectPromise_->setValue(folly::unit);
    co_return session_;
  } catch (const std::exception& ex) {
    XLOG(ERR) << "UpstreamProvider: connection failed: " << ex.what();
    state_ = State::Disconnected;
    connectPromise_->setException(
        folly::exception_wrapper(std::current_exception()));
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
      verifier_);

  quic::TransportSettings ts;
  ts.orderedReadCallbacks = true;

  co_await client_->setupMoQSession(
      std::chrono::milliseconds(5000),
      std::chrono::milliseconds(5000),
      publishHandler_,
      subscribeHandler_,
      ts);

  session_ = client_->moqSession_;
  CHECK(session_) << "setupMoQSession succeeded but session is null";

  // Register for close notifications
  session_->setSessionCloseCallback(this);

  // Relay chaining requires draft 16+ for wildcard subscribeNamespace (empty
  // prefix) and NAMESPACE messages on the bidi stream. Fail without retry if
  // the upstream negotiates an earlier draft — misconfiguration, not transient.
  if (!relayID_.empty()) {
    auto maybeVersion = session_->getNegotiatedVersion();
    if (!maybeVersion || getDraftMajorVersion(*maybeVersion) < 16) {
      auto ver = maybeVersion ? std::to_string(getDraftMajorVersion(*maybeVersion))
                              : std::string("unknown");
      XLOG(ERR) << "UpstreamProvider: upstream negotiated draft " << ver
                << " but relay chaining requires draft 16+; "
                   "disabling peering (no namespace sync)";
      // Leave relayID_ set so stop() cleans up, but skip the peer subNs.
      co_return;
    }
  }

  // Relay peering handshake: subscribe to all namespaces with relay auth token.
  // The upstream relay recognises the token and reciprocates, populating our
  // local namespace tree via the existing announcement/publish machinery.
  if (!relayID_.empty()) {
    XLOG(DBG1) << "UpstreamProvider: issuing peer subNs, relayID=" << relayID_;
    // Bridge NAMESPACE/NAMESPACE_DONE messages (draft 16+) back into the local
    // relay via subscribeHandler_. If subscribeHandler_ is absent, fall back to
    // a no-op handle (namespace announcements arrive via PUBLISH_NAMESPACE only).
    auto handle = makeNamespaceBridgeHandle(subscribeHandler_, session_);
    co_await session_->subscribeNamespace(makePeerSubNs(relayID_), handle);
  }

  XLOG(DBG1) << "UpstreamProvider::doConnect completed, session="
             << session_.get();
}

void UpstreamProvider::resetSession() {
  XLOG(DBG1) << "UpstreamProvider::resetSession";
  if (session_) {
    session_->setSessionCloseCallback(nullptr);
  }
  session_.reset();
  client_.reset();
  state_ = State::Disconnected;
}

} // namespace openmoq::moqx
