/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MoQIntegrationTestFixture.h"
#include "MoqxRelay.h"
#include "relay/PublisherCrossExecFilter.h"
#include "relay/SessionParticipant.h"
#include "relay/SubscriberCrossExecFilter.h"

#include <folly/coro/BlockingWait.h>
#include <folly/coro/Task.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <moxygen/MoQSession.h>
#include <moxygen/events/MoQFollyExecutorImpl.h>
#include <moxygen/test/Mocks.h>

#include <chrono>
#include <thread>

using namespace moxygen;
using namespace moxygen::test;
using namespace testing;

namespace openmoq::moqx::test {

namespace {

const TrackNamespace kTestNamespace{{"test", "ns"}};
const FullTrackName kTestTrack{kTestNamespace, "track"};

// Real MoQ sessions connect to a relay, then close without ending their requests first. The
// relay must clean up through the sessions' own teardown.
class RelaySessionCloseTest : public MoQIntegrationTestFixture {
protected:
  std::shared_ptr<Publisher> createServerPublishHandler() override {
    participant_ = std::make_shared<SessionParticipant>(
        std::make_shared<PublisherCrossExecFilter>(relayExec_.get(), relay_),
        std::make_shared<SubscriberCrossExecFilter>(relayExec_.get(), relay_),
        relayExec_.get()
    );
    return participant_;
  }
  std::shared_ptr<Subscriber> createServerSubscribeHandler() override { return participant_; }

  void SetUp() override {
    relayExec_ = std::make_shared<MoQFollyExecutorImpl>(relayThread_.getEventBase());
    relay_ = std::make_shared<MoqxRelay>(
        config::CacheConfig{.maxCachedTracks = 0},
        /*relayID=*/"",
        relayExec_
    );
    relay_->setAllowedNamespacePrefix(TrackNamespace{});
    MoQIntegrationTestFixture::SetUp();

    upstream_ = std::make_shared<NiceMock<MockPublisher>>();
    ON_CALL(*upstream_, subscribe(_, _))
        .WillByDefault(
            [this](SubscribeRequest sub, std::shared_ptr<TrackConsumer>)
                -> folly::coro::Task<Publisher::SubscribeResult> {
              SubscribeOk ok;
              ok.requestID = sub.requestID;
              ok.trackAlias = TrackAlias(sub.requestID.value);
              ok.expires = std::chrono::milliseconds(0);
              ok.groupOrder = GroupOrder::OldestFirst;
              upstreamHandle_ = std::make_shared<NiceMock<MockSubscriptionHandle>>(std::move(ok));
              ON_CALL(*upstreamHandle_, unsubscribe()).WillByDefault([this] {
                unsubscribed_ = true;
              });
              co_return upstreamHandle_;
            }
        );

    publisher_ = connect(upstream_);
    subscriber_ = connect(nullptr);
    publishNamespace();
    subscribe();
  }

  void TearDown() override {
    for (auto* client : {&publisher_, &subscriber_}) {
      if (*client && (*client)->moqSession_) {
        (*client)->moqSession_->close(SessionCloseErrorCode::NO_ERROR);
      }
    }
    MoQIntegrationTestFixture::TearDown();
    publisher_.reset();
    subscriber_.reset();
  }

  std::unique_ptr<MoQClient> connect(std::shared_ptr<Publisher> publishHandler) {
    auto client = createClient();
    folly::coro::blockingWait(
        client->setupMoQSession(
            std::chrono::seconds(2),
            std::chrono::seconds(2),
            std::move(publishHandler),
            nullptr,
            quic::TransportSettings(),
            getMoqtProtocols("16", /*useStandard=*/true)
        ),
        &clientEvb()
    );
    return client;
  }

  void publishNamespace() {
    PublishNamespace pubNs;
    pubNs.trackNamespace = kTestNamespace;
    auto result = folly::coro::blockingWait(
        publisher_->moqSession_->publishNamespace(std::move(pubNs), nullptr),
        &clientEvb()
    );
    ASSERT_TRUE(result.hasValue());
    publishNamespaceHandle_ = std::move(result.value());
  }

  void subscribe() {
    consumer_ = std::make_shared<NiceMock<MockTrackConsumer>>();
    ON_CALL(*consumer_, setTrackAlias(_))
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
    ON_CALL(*consumer_, publishDone(_)).WillByDefault([this](PublishDone) {
      publishDone_ = true;
      return folly::makeExpected<MoQPublishError>(folly::unit);
    });
    auto result = folly::coro::blockingWait(
        subscriber_->moqSession_->subscribe(
            SubscribeRequest::make(
                kTestTrack,
                kDefaultPriority,
                GroupOrder::OldestFirst,
                /*forward=*/true,
                LocationType::LargestObject
            ),
            consumer_
        ),
        &clientEvb()
    );
    ASSERT_TRUE(result.hasValue());
    subscriptionHandle_ = std::move(result.value());
  }

  // Runs the client loop until done() holds, or about two seconds pass.
  template <typename Fn> bool waitFor(Fn done) {
    for (int i = 0; i < 2000 && !done(); ++i) {
      clientEvb().loopOnce(EVLOOP_NONBLOCK);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done();
  }

  bool namespaceRouted() {
    bool routed = false;
    relayThread_.getEventBase()->runInEventBaseThreadAndWait([&] {
      routed = relay_->findNamespacePublisher(kTestNamespace).has_value();
    });
    return routed;
  }

  folly::ScopedEventBaseThread relayThread_{"relay"};
  std::shared_ptr<MoQFollyExecutorImpl> relayExec_;
  std::shared_ptr<MoqxRelay> relay_;
  std::shared_ptr<SessionParticipant> participant_;

  std::shared_ptr<NiceMock<MockPublisher>> upstream_;
  std::shared_ptr<NiceMock<MockSubscriptionHandle>> upstreamHandle_;
  bool unsubscribed_{false};
  std::shared_ptr<NiceMock<MockTrackConsumer>> consumer_;
  bool publishDone_{false};

  std::unique_ptr<MoQClient> publisher_;
  std::unique_ptr<MoQClient> subscriber_;
  std::shared_ptr<Subscriber::PublishNamespaceHandle> publishNamespaceHandle_;
  std::shared_ptr<Publisher::SubscriptionHandle> subscriptionHandle_;
};

TEST_F(RelaySessionCloseTest, SubscriberCloseEndsTheUpstreamSubscription) {
  ASSERT_NE(upstreamHandle_, nullptr);
  subscriber_->moqSession_->close(SessionCloseErrorCode::NO_ERROR);
  EXPECT_TRUE(waitFor([&] { return unsubscribed_; }));
  EXPECT_TRUE(namespaceRouted());
}

TEST_F(RelaySessionCloseTest, PublisherCloseRemovesItsNamespaceAndEndsSubscriptions) {
  ASSERT_TRUE(namespaceRouted());
  publisher_->moqSession_->close(SessionCloseErrorCode::NO_ERROR);
  EXPECT_TRUE(waitFor([&] { return publishDone_ && !namespaceRouted(); }));
  EXPECT_FALSE(namespaceRouted());
  EXPECT_TRUE(publishDone_);
}

} // namespace

} // namespace openmoq::moqx::test
