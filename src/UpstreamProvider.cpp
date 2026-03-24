/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <o_rly/UpstreamProvider.h>
#include <moxygen/MoQFilters.h>
#include <moxygen/MoQRelaySession.h>
#include <folly/logging/xlog.h>

using namespace moxygen;

namespace openmoq::o_rly {

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
    std::shared_ptr<fizz::CertificateVerifier> verifier)
    : publishHandler_(std::move(publishHandler)),
      subscribeHandler_(std::move(subscribeHandler)),
      url_(std::move(url)),
      exec_(std::move(exec)),
      verifier_(std::move(verifier)) {
  XLOG(DBG1) << "UpstreamProvider created, url=" << url_.getUrl();
}

UpstreamProvider::~UpstreamProvider() {
  XLOG(DBG1) << "UpstreamProvider destroyed";
}

folly::coro::Task<void> UpstreamProvider::start() {
  XLOG(DBG1) << "UpstreamProvider::start";
  co_await getOrConnectSession();
  XLOG(DBG1) << "UpstreamProvider::start completed, session=" << session_.get();
}

void UpstreamProvider::stop() {
  XLOG(DBG1) << "UpstreamProvider::stop";
  stopped_ = true;

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

  // Update URL if a new session URI is provided
  if (!goaway.newSessionUri.empty()) {
    XLOG(INFO) << "UpstreamProvider: updating URL from goaway: "
               << goaway.newSessionUri;
    url_ = proxygen::URL(goaway.newSessionUri);
  }

  // Reset session state - next operation will trigger reconnection
  resetSession();
}

// --- MoQSessionCloseCallback ---

void UpstreamProvider::onMoQSessionClosed() {
  XLOG(INFO) << "UpstreamProvider::onMoQSessionClosed";
  resetSession();
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

} // namespace openmoq::o_rly
