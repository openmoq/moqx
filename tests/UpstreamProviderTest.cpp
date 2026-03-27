/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MoQIntegrationTestFixture.h"
#include <o_rly/UpstreamProvider.h>

#include <folly/coro/BlockingWait.h>
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

namespace openmoq::o_rly::test {

namespace {

const TrackNamespace kTestNamespace{{"test", "ns"}};
const FullTrackName kTestTrack{kTestNamespace, "track1"};

Publisher::SubscribeResult makeSubscribeOk(const SubscribeRequest& sub) {
  SubscribeOk ok;
  ok.requestID = sub.requestID;
  ok.trackAlias = TrackAlias(0);
  ok.expires = std::chrono::milliseconds(0);
  ok.groupOrder = GroupOrder::OldestFirst;
  return std::make_shared<MockSubscriptionHandle>(std::move(ok));
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

  // Expect exactly one subscribe call that returns SubscribeOk.
  void expectSubscribe() {
    EXPECT_CALL(*serverPublisher_, subscribe(_, _))
        .WillOnce(
            [](SubscribeRequest sub,
               std::shared_ptr<TrackConsumer>) -> folly::coro::Task<Publisher::SubscribeResult> {
              co_return makeSubscribeOk(sub);
            }
        );
  }

  // Allow any number of subscribe calls, each returning SubscribeOk.
  void allowSubscribe() {
    EXPECT_CALL(*serverPublisher_, subscribe(_, _))
        .WillRepeatedly(
            [](SubscribeRequest sub,
               std::shared_ptr<TrackConsumer>) -> folly::coro::Task<Publisher::SubscribeResult> {
              co_return makeSubscribeOk(sub);
            }
        );
  }

  // Expect exactly one publishNamespace call that returns PublishNamespaceOk.
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

TEST_F(UpstreamProviderTest, SessionCloseTriggersLazyReconnect) {
  allowSubscribe();

  folly::coro::blockingWait(
      [&]() -> folly::coro::Task<void> {
        // Connect via first subscribe, then grab the session
        co_await provider_->subscribe(makeSubscribeRequest(), makeReceiver());
        auto firstSession = provider_->currentSession();
        EXPECT_NE(firstSession, nullptr);

        firstSession->close(SessionCloseErrorCode::NO_ERROR);
        co_await folly::coro::sleep(std::chrono::milliseconds(100));
        EXPECT_EQ(provider_->currentSession(), nullptr);

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

} // namespace openmoq::o_rly::test
