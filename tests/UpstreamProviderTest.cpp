/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MoQIntegrationTestFixture.h"
#include <moqx/UpstreamProvider.h>

#include <folly/coro/BlockingWait.h>
#include <folly/coro/Collect.h>
#include <folly/coro/Sleep.h>
#include <folly/coro/Task.h>
#include <folly/logging/xlog.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <moxygen/MoQSession.h>
#include <moxygen/ObjectReceiver.h>
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

// --- Pure function tests: isPeerSubNs / makePeerSubNs ---
// These don't need a fixture or event loop.

TEST(PeerSubNsHelpers, PeerSubNsRoundTrip) {
  // A subNs built with a relayID is detected as a peer subNs; without it is not.
  auto withID = makePeerSubNs("my-relay-id");
  EXPECT_TRUE(isPeerSubNs(withID));
  EXPECT_EQ(withID.trackNamespacePrefix, TrackNamespace{});
  EXPECT_EQ(withID.options, SubscribeNamespaceOptions::BOTH);

  auto withoutID = makePeerSubNs();
  EXPECT_FALSE(isPeerSubNs(withoutID));
}

TEST(PeerSubNsHelpers, NormalSubNsIsNotPeer) {
  SubscribeNamespace subNs;
  subNs.trackNamespacePrefix = TrackNamespace{{"test"}};
  subNs.options = SubscribeNamespaceOptions::BOTH;
  EXPECT_FALSE(isPeerSubNs(subNs));
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
            EXPECT_TRUE(isPeerSubNs(subNs));
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

} // namespace openmoq::moqx::test
