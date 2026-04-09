/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "UpstreamProvider.h"
#include "MoQIntegrationTestFixture.h"
#include "MoqxRelay.h"

#include <folly/coro/BlockingWait.h>
#include <folly/coro/Collect.h>
#include <folly/coro/Sleep.h>
#include <folly/coro/Task.h>
#include <folly/logging/xlog.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <moxygen/MoQSession.h>
#include <moxygen/MoQVersions.h>
#include <moxygen/ObjectReceiver.h>
#include <moxygen/test/MockMoQSession.h>
#include <moxygen/test/Mocks.h>

using namespace moxygen;
using namespace testing;

namespace openmoq::moqx::test {

namespace {

const TrackNamespace kTestNamespace{{"test", "ns"}};
const FullTrackName kTestTrack{kTestNamespace, "track1"};

Publisher::SubscribeResult makeSubscribeOk(const SubscribeRequest& sub) {
  SubscribeOk ok;
  ok.requestID = sub.requestID;
  ok.trackAlias = TrackAlias(sub.requestID.value);
  ok.expires = std::chrono::milliseconds(0);
  ok.groupOrder = GroupOrder::OldestFirst;
  return std::make_shared<MockSubscriptionHandle>(std::move(ok));
}

Publisher::FetchResult makeFetchOk(const Fetch& fetch) {
  FetchOk ok;
  ok.requestID = fetch.requestID;
  ok.groupOrder = GroupOrder::OldestFirst;
  ok.endOfTrack = 1;
  return std::make_shared<MockFetchHandle>(std::move(ok));
}

Publisher::SubscribeNamespaceResult makeSubscribeNamespaceOk(const SubscribeNamespace& subNs) {
  return std::make_shared<MockSubscribeNamespaceHandle>(SubscribeNamespaceOk{subNs.requestID});
}

Publisher::TrackStatusResult makeTrackStatusOk(const TrackStatus& req) {
  TrackStatusOk ok;
  ok.requestID = req.requestID;
  ok.trackAlias = TrackAlias(req.requestID.value);
  ok.statusCode = TrackStatusCode::IN_PROGRESS;
  return ok;
}

folly::coro::Task<folly::Expected<PublishOk, PublishError>> makePublishOkTask(RequestID reqID) {
  PublishOk ok;
  ok.requestID = reqID;
  co_return ok;
}

Subscriber::PublishResult makePublishOk(const PublishRequest& pub) {
  auto consumer = std::make_shared<NiceMock<MockTrackConsumer>>();
  ON_CALL(*consumer, setTrackAlias(_))
      .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
  ON_CALL(*consumer, publishDone(_))
      .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
  return Subscriber::PublishConsumerAndReplyTask{
      std::move(consumer),
      makePublishOkTask(pub.requestID),
  };
}

struct NoopObjectCallback : public ObjectReceiverCallback {
  FlowControlState onObject(std::optional<TrackAlias>, const ObjectHeader&, Payload) override {
    return FlowControlState::UNBLOCKED;
  }
  void onObjectStatus(std::optional<TrackAlias>, const ObjectHeader&) override {}
  void onEndOfStream() override {}
  void onError(ResetStreamErrorCode) override {}
  void onPublishDone(PublishDone) override {}
};

} // namespace

// --- Pure function tests: getPeerRelayID / makePeerSubNs ---
// These don't need a fixture or event loop.

TEST(PeerSubNsHelpers, PeerSubNsRoundTrip) {
  // A subNs built with a relayID returns that ID; without one returns nullopt.
  auto withID = makePeerSubNs("my-relay-id");
  auto id = getPeerRelayID(withID);
  EXPECT_TRUE(id.has_value());
  EXPECT_EQ(*id, "my-relay-id");
  EXPECT_EQ(withID.trackNamespacePrefix, TrackNamespace{});
  EXPECT_EQ(withID.options, SubscribeNamespaceOptions::BOTH);

  auto withoutID = makePeerSubNs();
  EXPECT_FALSE(getPeerRelayID(withoutID).has_value());
}

TEST(PeerSubNsHelpers, NormalSubNsIsNotPeer) {
  SubscribeNamespace subNs;
  subNs.trackNamespacePrefix = TrackNamespace{{"test"}};
  subNs.options = SubscribeNamespaceOptions::BOTH;
  EXPECT_FALSE(getPeerRelayID(subNs).has_value());
}

// --- Integration fixture tests ---
// All coroutine tests must use blockingWait(..., &clientEvb()) because the
// provider and its session are bound to that event loop. CO_TEST_F cannot be
// used here.

class UpstreamProviderTest : public MoQIntegrationTestFixture {
protected:
  std::shared_ptr<Publisher> createServerPublishHandler() override { return serverPublisher_; }

  std::shared_ptr<Subscriber> createServerSubscribeHandler() override { return serverSubscriber_; }

  void SetUp() override {
    serverPublisher_ = std::make_shared<NiceMock<MockPublisher>>();
    serverSubscriber_ = std::make_shared<NiceMock<MockSubscriber>>();
    MoQIntegrationTestFixture::SetUp();
    provider_ = std::make_shared<UpstreamProvider>(
        clientExec(),
        serverUrl(),
        /*publishHandler=*/nullptr,
        /*subscribeHandler=*/nullptr,
        std::make_shared<moxygen::test::InsecureVerifierDangerousDoNotUseInProduction>()
    );
  }

  void TearDown() override {
    provider_->stop();
    MoQIntegrationTestFixture::TearDown();
  }

  std::shared_ptr<ObjectReceiver> makeReceiver() {
    return std::make_shared<ObjectReceiver>(
        ObjectReceiver::SUBSCRIBE,
        std::make_shared<NoopObjectCallback>()
    );
  }

  SubscribeRequest makeSubscribeRequest() {
    return SubscribeRequest::make(
        kTestTrack,
        kDefaultPriority,
        GroupOrder::OldestFirst,
        /*forward=*/true,
        LocationType::LargestObject
    );
  }

  Fetch makeFetchRequest() {
    return Fetch(RequestID(2), kTestTrack, AbsoluteLocation{0, 0}, AbsoluteLocation{100, 100});
  }

  void expectSubscribe() {
    EXPECT_CALL(*serverPublisher_, subscribe(_, _))
        .WillOnce(
            [](SubscribeRequest sub,
               std::shared_ptr<TrackConsumer>) -> folly::coro::Task<Publisher::SubscribeResult> {
              co_return makeSubscribeOk(sub);
            }
        );
  }

  void allowSubscribe() {
    EXPECT_CALL(*serverPublisher_, subscribe(_, _))
        .WillRepeatedly(
            [](SubscribeRequest sub,
               std::shared_ptr<TrackConsumer>) -> folly::coro::Task<Publisher::SubscribeResult> {
              co_return makeSubscribeOk(sub);
            }
        );
  }

  void expectPublishNamespace() {
    EXPECT_CALL(*serverSubscriber_, publishNamespace(_, _))
        .WillOnce(
            [](PublishNamespace pubNs, std::shared_ptr<Subscriber::PublishNamespaceCallback>)
                -> folly::coro::Task<Subscriber::PublishNamespaceResult> {
              co_return std::make_shared<MockPublishNamespaceHandle>(
                  PublishNamespaceOk{pubNs.requestID}
              );
            }
        );
  }

  std::shared_ptr<UpstreamProvider> provider_;
  std::shared_ptr<NiceMock<MockPublisher>> serverPublisher_;
  std::shared_ptr<NiceMock<MockSubscriber>> serverSubscriber_;
};

TEST_F(UpstreamProviderTest, ConnectAndSetup) {
  folly::coro::blockingWait(
      [&]() -> folly::coro::Task<void> {
        co_await provider_->start();
        EXPECT_NE(provider_->currentSession(), nullptr);
      }(),
      &clientEvb()
  );
}

// Exercises slow path (coSubscribe): not yet connected when subscribe is called.
TEST_F(UpstreamProviderTest, SubscribeForwardsToUpstream) {
  expectSubscribe();

  folly::coro::blockingWait(
      [&]() -> folly::coro::Task<void> {
        auto result = co_await provider_->subscribe(makeSubscribeRequest(), makeReceiver());
        EXPECT_TRUE(result.hasValue());
      }(),
      &clientEvb()
  );
}

// Exercises fast path (getSession()): already connected when subscribe is called.
TEST_F(UpstreamProviderTest, SubscribeFastPathAfterConnect) {
  allowSubscribe();

  folly::coro::blockingWait(
      [&]() -> folly::coro::Task<void> {
        co_await provider_->start();
        auto result = co_await provider_->subscribe(makeSubscribeRequest(), makeReceiver());
        EXPECT_TRUE(result.hasValue());
      }(),
      &clientEvb()
  );
}

// Two subscribes fired before connected: both wait on the same connectPromise_
// and succeed after a single connection is established.
TEST_F(UpstreamProviderTest, ConcurrentSubscribesWhileConnecting) {
  EXPECT_CALL(*serverPublisher_, subscribe(_, _))
      .Times(2)
      .WillRepeatedly(
          [](SubscribeRequest sub,
             std::shared_ptr<TrackConsumer>) -> folly::coro::Task<Publisher::SubscribeResult> {
            co_return makeSubscribeOk(sub);
          }
      );

  folly::coro::blockingWait(
      [&]() -> folly::coro::Task<void> {
        auto [r1, r2] = co_await folly::coro::collectAll(
            provider_->subscribe(makeSubscribeRequest(), makeReceiver()),
            provider_->subscribe(makeSubscribeRequest(), makeReceiver())
        );
        EXPECT_TRUE(r1.hasValue());
        EXPECT_TRUE(r2.hasValue());
        EXPECT_NE(provider_->currentSession(), nullptr);
      }(),
      &clientEvb()
  );
}

TEST_F(UpstreamProviderTest, FetchForwardsToUpstream) {
  EXPECT_CALL(*serverPublisher_, fetch(_, _))
      .WillOnce(
          [](Fetch fetch,
             std::shared_ptr<FetchConsumer>) -> folly::coro::Task<Publisher::FetchResult> {
            co_return makeFetchOk(fetch);
          }
      );

  folly::coro::blockingWait(
      [&]() -> folly::coro::Task<void> {
        auto result = co_await provider_->fetch(makeFetchRequest(), nullptr);
        EXPECT_TRUE(result.hasValue());
      }(),
      &clientEvb()
  );
}

TEST_F(UpstreamProviderTest, SubscribeNamespaceForwardsToUpstream) {
  EXPECT_CALL(*serverPublisher_, subscribeNamespace(_, _))
      .WillOnce(
          [](SubscribeNamespace subNs, std::shared_ptr<Publisher::NamespacePublishHandle>)
              -> folly::coro::Task<Publisher::SubscribeNamespaceResult> {
            co_return makeSubscribeNamespaceOk(subNs);
          }
      );

  folly::coro::blockingWait(
      [&]() -> folly::coro::Task<void> {
        SubscribeNamespace subNs;
        subNs.requestID = RequestID(3);
        subNs.trackNamespacePrefix = kTestNamespace;
        subNs.options = SubscribeNamespaceOptions::BOTH;
        auto result = co_await provider_->subscribeNamespace(std::move(subNs), nullptr);
        EXPECT_TRUE(result.hasValue());
      }(),
      &clientEvb()
  );
}

TEST_F(UpstreamProviderTest, PublishNamespaceForwardsToUpstream) {
  expectPublishNamespace();

  folly::coro::blockingWait(
      [&]() -> folly::coro::Task<void> {
        PublishNamespace pubNs;
        pubNs.requestID = RequestID(1);
        pubNs.trackNamespace = kTestNamespace;
        auto result = co_await provider_->publishNamespace(std::move(pubNs), nullptr);
        EXPECT_TRUE(result.hasValue());
      }(),
      &clientEvb()
  );
}

// When relayID is set, UpstreamProvider issues a wildcard subscribeNamespace
// with a peer auth token to the upstream on connect.
TEST_F(UpstreamProviderTest, PeerSubNsHandshakeOnConnect) {
  EXPECT_CALL(*serverPublisher_, subscribeNamespace(_, _))
      .WillOnce(
          [](SubscribeNamespace subNs, std::shared_ptr<Publisher::NamespacePublishHandle>)
              -> folly::coro::Task<Publisher::SubscribeNamespaceResult> {
            EXPECT_TRUE(getPeerRelayID(subNs).has_value());
            EXPECT_EQ(subNs.trackNamespacePrefix, TrackNamespace{});
            co_return makeSubscribeNamespaceOk(subNs);
          }
      );

  // Rebuild provider_ with the onConnect hook so TearDown handles cleanup.
  // The default provider was never started (session_==null), so no stop() needed.
  provider_ = std::make_shared<UpstreamProvider>(
      clientExec(),
      serverUrl(),
      /*publishHandler=*/nullptr,
      /*subscribeHandler=*/nullptr,
      std::make_shared<moxygen::test::InsecureVerifierDangerousDoNotUseInProduction>(),
      /*onConnect=*/
      [](std::shared_ptr<MoQSession> session) -> folly::coro::Task<void> {
        auto result = co_await session->subscribeNamespace(makePeerSubNs("test-relay-id"), nullptr);
        EXPECT_TRUE(result.hasValue());
      }
  );

  folly::coro::blockingWait(
      [&]() -> folly::coro::Task<void> {
        co_await provider_->start();
        EXPECT_NE(provider_->currentSession(), nullptr);
      }(),
      &clientEvb()
  );
}

TEST_F(UpstreamProviderTest, StopFailsPendingOperations) {
  folly::coro::blockingWait(
      [&]() -> folly::coro::Task<void> {
        co_await provider_->start();
        provider_->stop();

        bool threw = false;
        try {
          co_await provider_->subscribe(makeSubscribeRequest(), makeReceiver());
        } catch (const std::runtime_error& e) {
          threw = true;
          XLOG(DBG1) << "Expected error: " << e.what();
        }
        EXPECT_TRUE(threw);
      }(),
      &clientEvb()
  );
}

TEST_F(UpstreamProviderTest, SessionCloseTriggersProactiveReconnect) {
  allowSubscribe();

  folly::coro::blockingWait(
      [&]() -> folly::coro::Task<void> {
        co_await provider_->subscribe(makeSubscribeRequest(), makeReceiver());
        auto firstSession = provider_->currentSession();
        EXPECT_NE(firstSession, nullptr);

        // Close the session; provider proactively starts a reconnect loop.
        firstSession->close(SessionCloseErrorCode::NO_ERROR);

        // After re-subscribing the provider should have a new session.
        auto result = co_await provider_->subscribe(makeSubscribeRequest(), makeReceiver());
        EXPECT_TRUE(result.hasValue());
        EXPECT_NE(provider_->currentSession(), nullptr);
        EXPECT_NE(provider_->currentSession(), firstSession);
      }(),
      &clientEvb()
  );
}

TEST_F(UpstreamProviderTest, GoawayResetsSession) {
  allowSubscribe();

  folly::coro::blockingWait(
      [&]() -> folly::coro::Task<void> {
        co_await provider_->subscribe(makeSubscribeRequest(), makeReceiver());
        auto firstSession = provider_->currentSession();
        EXPECT_NE(firstSession, nullptr);

        provider_->goaway(Goaway{""});
        EXPECT_EQ(provider_->currentSession(), nullptr);

        auto result = co_await provider_->subscribe(makeSubscribeRequest(), makeReceiver());
        EXPECT_TRUE(result.hasValue());
        EXPECT_NE(provider_->currentSession(), nullptr);
      }(),
      &clientEvb()
  );
}

TEST_F(UpstreamProviderTest, PublishFailsWhenStopped) {
  provider_->stop();

  PublishRequest pub;
  pub.requestID = RequestID(1);
  pub.fullTrackName = kTestTrack;
  pub.groupOrder = GroupOrder::OldestFirst;

  auto result = provider_->publish(std::move(pub), nullptr);
  EXPECT_TRUE(result.hasError());
  EXPECT_EQ(result.error().errorCode, PublishErrorCode::INTERNAL_ERROR);
}

// --- trackStatus ---

TEST_F(UpstreamProviderTest, TrackStatusForwardsToUpstream) {
  EXPECT_CALL(*serverPublisher_, trackStatus(_))
      .WillOnce([](TrackStatus req) -> folly::coro::Task<Publisher::TrackStatusResult> {
        co_return makeTrackStatusOk(req);
      });

  folly::coro::blockingWait(
      [&]() -> folly::coro::Task<void> {
        auto result = co_await provider_->trackStatus(makeSubscribeRequest());
        EXPECT_TRUE(result.hasValue());
      }(),
      &clientEvb()
  );
}

TEST_F(UpstreamProviderTest, TrackStatusFastPathAfterConnect) {
  EXPECT_CALL(*serverPublisher_, trackStatus(_))
      .WillOnce([](TrackStatus req) -> folly::coro::Task<Publisher::TrackStatusResult> {
        co_return makeTrackStatusOk(req);
      });

  folly::coro::blockingWait(
      [&]() -> folly::coro::Task<void> {
        co_await provider_->start();
        auto result = co_await provider_->trackStatus(makeSubscribeRequest());
        EXPECT_TRUE(result.hasValue());
      }(),
      &clientEvb()
  );
}

// --- publish() ---

// publish() when already connected: synchronous fast-path via session_->publish().
TEST_F(UpstreamProviderTest, PublishFastPathAfterConnect) {
  EXPECT_CALL(*serverSubscriber_, publish(_, _))
      .WillOnce(
          [](PublishRequest pub, std::shared_ptr<SubscriptionHandle>) -> Subscriber::PublishResult {
            return makePublishOk(pub);
          }
      );

  folly::coro::blockingWait(
      [&]() -> folly::coro::Task<void> {
        co_await provider_->start();

        PublishRequest pub;
        pub.requestID = RequestID(1);
        pub.fullTrackName = kTestTrack;
        pub.groupOrder = GroupOrder::OldestFirst;

        auto result = provider_->publish(
            pub,
            std::make_shared<NiceMock<MockSubscriptionHandle>>(SubscribeOk{})
        );
        EXPECT_TRUE(result.hasValue());
        if (!result.hasValue()) {
          co_return;
        }
        auto ok = co_await std::move(result.value().reply);
        EXPECT_TRUE(ok.hasValue());
      }(),
      &clientEvb()
  );
}

// publish() before connected: PendingTrackConsumer proxy that connects and wires
// setDownstream() before any forwarding can happen.
TEST_F(UpstreamProviderTest, PublishPreConnectPath) {
  EXPECT_CALL(*serverSubscriber_, publish(_, _))
      .WillOnce(
          [](PublishRequest pub, std::shared_ptr<SubscriptionHandle>) -> Subscriber::PublishResult {
            return makePublishOk(pub);
          }
      );

  PublishRequest pub;
  pub.requestID = RequestID(1);
  pub.fullTrackName = kTestTrack;
  pub.groupOrder = GroupOrder::OldestFirst;

  // Not yet connected — returns PendingTrackConsumer + reply task immediately.
  auto result =
      provider_->publish(pub, std::make_shared<NiceMock<MockSubscriptionHandle>>(SubscribeOk{}));
  ASSERT_TRUE(result.hasValue());

  // Driving the reply task triggers connect and returns PublishOk.
  folly::coro::blockingWait(
      [reply = std::move(result.value().reply)]() mutable -> folly::coro::Task<void> {
        auto ok = co_await std::move(reply);
        EXPECT_TRUE(ok.hasValue());
      }(),
      &clientEvb()
  );
}

// --- waitForConnected ---

TEST_F(UpstreamProviderTest, WaitForConnectedAlreadyConnected) {
  folly::coro::blockingWait(
      [&]() -> folly::coro::Task<void> {
        co_await provider_->start();
        // Already connected — must return immediately rather than blocking until timeout.
        co_await provider_->waitForConnected(std::chrono::milliseconds(100));
        EXPECT_NE(provider_->currentSession(), nullptr);
      }(),
      &clientEvb()
  );
}

// --- Shutdown / close paths ---

// stop() with no session established — dtor XCHECK (!session_ && !client_) must hold.
TEST_F(UpstreamProviderTest, StopBeforeStart) {
  provider_->stop();
  // TearDown calls stop() again; both must be safe.
}

// stop() is idempotent: calling it twice must not crash or double-free.
TEST_F(UpstreamProviderTest, StopIsIdempotent) {
  folly::coro::blockingWait(
      [&]() -> folly::coro::Task<void> { co_await provider_->start(); }(),
      &clientEvb()
  );
  provider_->stop();
  provider_->stop();
}

// onDisconnect hook fires when the upstream session is closed.
TEST_F(UpstreamProviderTest, OnDisconnectHookFires) {
  // Replace the default (never-started) provider with one that has the hook.
  bool fired = false;
  provider_ = std::make_shared<UpstreamProvider>(
      clientExec(),
      serverUrl(),
      /*publishHandler=*/nullptr,
      /*subscribeHandler=*/nullptr,
      std::make_shared<moxygen::test::InsecureVerifierDangerousDoNotUseInProduction>(),
      /*onConnect=*/nullptr,
      /*onDisconnect=*/[&fired]() { fired = true; }
  );

  folly::coro::blockingWait(
      [&]() -> folly::coro::Task<void> {
        co_await provider_->start();
        EXPECT_NE(provider_->currentSession(), nullptr);
        if (!provider_->currentSession()) {
          co_return;
        }
        provider_->currentSession()->close(SessionCloseErrorCode::NO_ERROR);
        co_await folly::coro::sleep(std::chrono::milliseconds(50));
        EXPECT_TRUE(fired);
      }(),
      &clientEvb()
  );
}

// stop() before a session close must not spawn a reconnect loop.
TEST_F(UpstreamProviderTest, StopSuppressesReconnectAfterSessionClose) {
  allowSubscribe();

  folly::coro::blockingWait(
      [&]() -> folly::coro::Task<void> {
        co_await provider_->start();
        auto session = provider_->currentSession();
        EXPECT_NE(session, nullptr);
        if (!session) {
          co_return;
        }

        provider_->stop();
        session->close(SessionCloseErrorCode::NO_ERROR);

        co_await folly::coro::sleep(std::chrono::milliseconds(50));
        EXPECT_EQ(provider_->currentSession(), nullptr);
      }(),
      &clientEvb()
  );
}

// ===== RelayUpstreamSubscribeRaceTest =============================================
//
// Regression test for the TOCTOU race in MoqxRelay::subscribe() that caused
// folly::PromiseAlreadySatisfied → std::terminate (crash of 2026-04-10).
//
// Root cause: two concurrent subscribers both pass subscriptions_.find()==end()
// before either calls subscriptions_.emplace(), because the only suspension
// point between the find() and emplace() is
//   co_await upstream_->waitForConnected().
// Both coroutines suspend there, both resume with a valid upstreamSession, both
// call emplace() for the same key (the second emplace silently returns the
// existing entry), and both eventually call rsub.promise.setValue() on the same
// subscriptions_ entry — the second call throws PromiseAlreadySatisfied.
//
// Fix: after waitForConnected() returns, re-check subscriptions_.find() and
// fall through to the second-subscriber path if another coroutine already
// emplaced the entry while we were waiting.
//
// Test setup:
//   provider_->start() is launched FIRST in collectAll so that it runs first,
//   creating connectPromise_ (state=Connecting) before the two subscribe tasks
//   start.  The QUIC handshake is async, so start() suspends at network I/O.
//   subscribeTask(1) and subscribeTask(2) then enter waitForConnected() and
//   block on connectPromise_.  After the handshake completes,
//   relay.onUpstreamConnect → session.subscribeNamespace; the server mock
//   calls nsHandle->namespaceMsg(kTestNamespace) which invokes
//   MoqxRelayNamespaceHandle::namespaceMsg → relay.doPublishNamespace
//   synchronously, so the namespace is in the relay tree before
//   connectPromise_ is fulfilled.  Both subscribe coroutines then resume with
//   a valid upstreamSession and the race is triggered.

class RelayUpstreamSubscribeRaceTest : public MoQIntegrationTestFixture {
protected:
  std::shared_ptr<Publisher> createServerPublishHandler() override { return serverPublisher_; }

  void SetUp() override {
    serverPublisher_ = std::make_shared<NiceMock<MockPublisher>>();
    MoQIntegrationTestFixture::SetUp();

    relay_ = std::make_shared<MoqxRelay>(config::CacheConfig{.maxCachedTracks = 0});
    relay_->setAllowedNamespacePrefix(TrackNamespace{}); // allow all namespaces

    auto onConnect = [relay = relay_](std::shared_ptr<MoQSession> session
                     ) -> folly::coro::Task<void> { co_await relay->onUpstreamConnect(session); };
    auto onDisconnect = [relay = relay_]() { relay->onUpstreamDisconnect(); };

    provider_ = std::make_shared<UpstreamProvider>(
        clientExec(),
        serverUrl(),
        relay_, // publishHandler: upstream NAMESPACE messages enter relay tree
        relay_, // subscribeHandler: relay's publishes forwarded upstream
        std::make_shared<moxygen::test::InsecureVerifierDangerousDoNotUseInProduction>(),
        std::move(onConnect),
        std::move(onDisconnect)
    );
    relay_->setUpstreamProvider(provider_);

    // Mock sessions representing two downstream subscribers.
    subSession1_ = std::make_shared<NiceMock<moxygen::test::MockMoQSession>>(clientExec());
    ON_CALL(*subSession1_, getNegotiatedVersion())
        .WillByDefault(Return(std::optional<uint64_t>(kVersionDraftCurrent)));
    subSession2_ = std::make_shared<NiceMock<moxygen::test::MockMoQSession>>(clientExec());
    ON_CALL(*subSession2_, getNegotiatedVersion())
        .WillByDefault(Return(std::optional<uint64_t>(kVersionDraftCurrent)));
  }

  void TearDown() override {
    relay_->stop();
    MoQIntegrationTestFixture::TearDown();
  }

  SubscribeRequest makeSubscribeRequest() {
    return SubscribeRequest::make(
        kTestTrack,
        kDefaultPriority,
        GroupOrder::OldestFirst,
        /*forward=*/true,
        LocationType::LargestObject
    );
  }

  // Runs relay_->subscribe() with `sess` set as the request session in the
  // folly RequestContext (required by MoQSession::getRequestSession() inside
  // the relay).  The RequestContextScopeGuard is a coroutine-local variable and
  // is preserved across suspension / resumption by folly's coro framework.
  folly::coro::Task<void> subscribeTask(std::shared_ptr<MoQSession> sess, SubscribeRequest req) {
    folly::RequestContextScopeGuard ctx;
    folly::RequestContext::get()->setContextData(
        folly::RequestToken("moq_session"),
        std::make_unique<MoQSession::MoQSessionRequestData>(std::move(sess))
    );
    auto consumer = std::make_shared<ObjectReceiver>(
        ObjectReceiver::SUBSCRIBE,
        std::make_shared<NoopObjectCallback>()
    );
    auto result = co_await relay_->subscribe(std::move(req), std::move(consumer));
    // With the fix: both subscribes succeed (the second falls through to the
    // second-subscriber path and waits for the first's promise).
    // Without the fix: std::terminate before we reach this line.
    EXPECT_TRUE(result.hasValue()) << "subscribe unexpectedly failed: "
                                   << (result.hasError() ? result.error().reasonPhrase : "");
  }

  std::shared_ptr<NiceMock<MockPublisher>> serverPublisher_;
  std::shared_ptr<MoqxRelay> relay_;
  std::shared_ptr<UpstreamProvider> provider_;
  std::shared_ptr<NiceMock<moxygen::test::MockMoQSession>> subSession1_;
  std::shared_ptr<NiceMock<moxygen::test::MockMoQSession>> subSession2_;
};

// Without the fix this test crashes with PromiseAlreadySatisfied → std::terminate.
// With the fix both subscribes complete successfully.
TEST_F(RelayUpstreamSubscribeRaceTest, ConcurrentSubscribesSameTrack) {
  // Server accepts the peer subNs sent by relay.onUpstreamConnect and
  // immediately announces kTestNamespace so it appears in the relay tree before
  // connectPromise_ is fulfilled.  The trackNamespacePrefix of the peer subNs
  // is {} (wildcard), so the suffix passed to namespaceMsg IS the full namespace.
  EXPECT_CALL(*serverPublisher_, subscribeNamespace(_, _))
      .WillOnce(
          [](SubscribeNamespace subNs, std::shared_ptr<Publisher::NamespacePublishHandle> nsHandle
          ) -> folly::coro::Task<Publisher::SubscribeNamespaceResult> {
            // Calling namespaceMsg here invokes
            // MoqxRelayNamespaceHandle::namespaceMsg → relay.doPublishNamespace
            // synchronously, so the namespace is in the relay tree by the time
            // subscribeNamespace returns (and before connectPromise_ is set).
            nsHandle->namespaceMsg(kTestNamespace);
            co_return makeSubscribeNamespaceOk(subNs);
          }
      );

  // Both relay→upstream SUBSCRIBE requests succeed.  Without the fix, both
  // coroutines reach this point for the same subscriptions_ entry and call
  // rsub.promise.setValue() twice → PromiseAlreadySatisfied → crash.
  EXPECT_CALL(*serverPublisher_, subscribe(_, _))
      .WillRepeatedly(
          [](SubscribeRequest sub,
             std::shared_ptr<TrackConsumer>) -> folly::coro::Task<Publisher::SubscribeResult> {
            co_return makeSubscribeOk(sub);
          }
      );

  SubscribeRequest subReq1 = makeSubscribeRequest();
  subReq1.requestID = RequestID(1);
  SubscribeRequest subReq2 = makeSubscribeRequest();
  subReq2.requestID = RequestID(2);

  folly::coro::blockingWait(
      [&]() -> folly::coro::Task<void> {
        // collectAll launch order is deterministic:
        //   [0] provider_->start(): suspends at QUIC handshake after setting
        //       state=Connecting and creating connectPromise_.
        //   [1] subscribeTask(1): enters waitForConnected(), blocks on
        //       connectPromise_ — connectPromise_ is now set (state=Connecting).
        //   [2] subscribeTask(2): same.
        // After the handshake, onUpstreamConnect fires, namespace enters the
        // relay tree, connectPromise_ is fulfilled, and both tasks resume.
        co_await folly::coro::collectAll(
            provider_->start(),
            subscribeTask(subSession1_, std::move(subReq1)),
            subscribeTask(subSession2_, std::move(subReq2))
        );
      }(),
      &clientEvb()
  );
}

} // namespace openmoq::moqx::test
