/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Originally from github.com/facebookexperimental/moxygen.
 * See deps/moxygen/LICENSE for the original license terms.
 *
 * Copyright (c) OpenMOQ contributors.
 */

#include "MoqxRelayTestFixture.h"

namespace moxygen::test {

TestMoQExecutor::TestMoQExecutor() : MoQFollyExecutorImpl(&evb_) {}
TestMoQExecutor::~TestMoQExecutor() = default;
void TestMoQExecutor::add(folly::Func func) {
  MoQFollyExecutorImpl::add(std::move(func));
}
void TestMoQExecutor::drive() {
  auto* evb = getBackingEventBase();
  if (!evb) {
    return;
  }
  evb->loopOnce();
  if (relayEvb_) {
    // flush our pending task to relay
    relayEvb_->runInEventBaseThreadAndWait([]() {});
    // now flush any tasks created by our task
    relayEvb_->runInEventBaseThreadAndWait([]() {});
    evb->loopOnce();
  }
}
void TestMoQExecutor::driveFor(int n) {
  for (int i = 0; i < n; i++) {
    drive();
  }
}

void MoQRelayTest::SetUp() {
  exec_ = std::make_shared<TestMoQExecutor>();
  relay_ = std::make_shared<MoqxRelay>(config::CacheConfig{.maxCachedTracks = 0});
  relay_->setAllowedNamespacePrefix(kAllowedPrefix);
  if (relayMode() == RelayMode::MultiThread) {
    relayThread_ = std::make_unique<folly::ScopedEventBaseThread>("relay-test");
    relayEvb_ = relayThread_->getEventBase();
    relay_->setRelayExec(relayEvb_);
    relay_->setUseLocalForwarders(false);
    exec_->setRelayEvb(relayEvb_);
    ASSERT_NE(relay_->getRelayExec(), nullptr);
    publisherInterface_ =
        std::make_shared<PublisherCrossExecFilter>(relay_->getRelayExec(), relay_);
    subscriberInterface_ =
        std::make_shared<SubscriberCrossExecFilter>(relay_->getRelayExec(), relay_);
  } else if (relayMode() == RelayMode::LocalForwarderMT) {
    relayThread_ = std::make_unique<folly::ScopedEventBaseThread>("relay-test");
    relayEvb_ = relayThread_->getEventBase();
    relay_->setRelayExec(relayEvb_);
    relay_->setUseLocalForwarders(true);
    exec_->setRelayEvb(relayEvb_);
    ASSERT_NE(relay_->getRelayExec(), nullptr);
    publisherInterface_ = relay_->createPublisherFilter();
    subscriberInterface_ = relay_->createSubscriberFilter();
  }
}

void MoQRelayTest::resetRelay(std::shared_ptr<MoqxRelay> relay) {
  relay_ = std::move(relay);
  if (relayEvb_) {
    relay_->setRelayExec(relayEvb_);
    if (relayMode() == RelayMode::LocalForwarderMT) {
      publisherInterface_ = relay_->createPublisherFilter();
      subscriberInterface_ = relay_->createSubscriberFilter();
    } else {
      publisherInterface_ = std::make_shared<PublisherCrossExecFilter>(relayEvb_, relay_);
      subscriberInterface_ = std::make_shared<SubscriberCrossExecFilter>(relayEvb_, relay_);
    }
  }
}

void MoQRelayTest::TearDown() {
  // Drain any pending relay exec tasks (e.g., async publishNamespaceDone dispatches
  // from cleanup) before destroying relay state to avoid use-after-free on NamespaceTree.
  if (relayEvb_) {
    relayEvb_->runInEventBaseThreadAndWait([]() {});
    relayEvb_->runInEventBaseThreadAndWait([]() {});
  }
  exec_->setRelayEvb(nullptr);
  publisherInterface_.reset();
  subscriberInterface_.reset();
  relay_.reset();
  relayThread_.reset();
}

std::shared_ptr<MockMoQSession> MoQRelayTest::createMockSession() {
  auto session = std::make_shared<NiceMock<MockMoQSession>>(exec_);
  ON_CALL(*session, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraftCurrent)));
  auto state = getOrCreateMockState(session);
  return session;
}

std::shared_ptr<Publisher::SubscriptionHandle> MoQRelayTest::createMockSubscriptionHandle() {
  SubscribeOk ok;
  ok.requestID = RequestID(0);
  ok.trackAlias = TrackAlias(0);
  ok.expires = std::chrono::milliseconds(0);
  ok.groupOrder = GroupOrder::Default;
  auto handle = std::make_shared<NiceMock<MockSubscriptionHandle>>(std::move(ok));
  return handle;
}

void MoQRelayTest::removeSession(std::shared_ptr<MoQSession> sess) {
  cleanupMockSession(std::move(sess));
}

std::shared_ptr<MockTrackConsumer> MoQRelayTest::createMockConsumer() {
  auto consumer = std::make_shared<NiceMock<MockTrackConsumer>>();
  ON_CALL(*consumer, setTrackAlias(_))
      .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
  ON_CALL(*consumer, publishDone(_))
      .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
  return consumer;
}

std::shared_ptr<SubscriptionHandle> MoQRelayTest::subscribeToTrack(
    std::shared_ptr<MoQSession> session,
    const FullTrackName& trackName,
    std::shared_ptr<TrackConsumer> consumer,
    RequestID requestID,
    bool addToState,
    folly::Optional<SubscribeErrorCode> expectedError
) {
  SubscribeRequest sub;
  sub.fullTrackName = trackName;
  sub.requestID = requestID;
  sub.locType = LocationType::LargestObject;
  std::shared_ptr<SubscriptionHandle> handle{nullptr};
  withSessionContext(session, [&]() {
    auto task = publisherInterface()->subscribe(std::move(sub), consumer);
    auto res = folly::coro::blockingWait(std::move(task), exec_.get());
    if (expectedError.has_value()) {
      EXPECT_FALSE(res.hasValue());
      const auto& err = res.error();
      EXPECT_EQ(err.errorCode, *expectedError);
    } else {
      EXPECT_TRUE(res.hasValue());
      handle = *res;
      if (addToState) {
        getOrCreateMockState(session)->subscribeHandles.push_back(handle);
      }
    }
  });
  return handle;
}

// static
const folly::RequestToken& MoQRelayTest::sessionRequestToken() {
  static folly::RequestToken token("moq_session");
  return token;
}

void MoQRelayTest::MockSessionState::cleanup() {
  for (auto& consumer : publishConsumers) {
    consumer->publishDone(
        {RequestID(0), PublishDoneStatusCode::SESSION_CLOSED, 0, "mock session cleanup"}
    );
  }
  publishConsumers.clear();

  for (auto& handle : publishNamespaceHandles) {
    if (handle) {
      handle->publishNamespaceDone();
    }
  }
  publishNamespaceHandles.clear();

  for (auto& handle : subscribeNamespaceHandles) {
    if (handle) {
      handle->unsubscribeNamespace();
    }
  }
  subscribeNamespaceHandles.clear();

  for (auto& handle : subscribeHandles) {
    if (handle) {
      handle->unsubscribe();
    }
  }
  subscribeHandles.clear();
}

std::shared_ptr<MoQRelayTest::MockSessionState>
MoQRelayTest::getOrCreateMockState(std::shared_ptr<MoQSession> session) {
  auto it = mockSessions_.find(session.get());
  if (it == mockSessions_.end()) {
    auto state = std::make_shared<MockSessionState>();
    state->session = session;
    mockSessions_[session.get()] = state;
    return state;
  }
  return it->second;
}

void MoQRelayTest::cleanupMockSession(std::shared_ptr<MoQSession> session) {
  auto it = mockSessions_.find(session.get());
  if (it != mockSessions_.end()) {
    withSessionContext(it->second->session, [&]() { it->second->cleanup(); });
    mockSessions_.erase(it);
  }
}

std::shared_ptr<Subscriber::PublishNamespaceHandle> MoQRelayTest::doPublishNamespace(
    std::shared_ptr<MoQSession> session,
    const TrackNamespace& ns,
    bool addToState
) {
  PublishNamespace ann;
  ann.trackNamespace = ns;
  return withSessionContext(session, [&]() {
    auto task = subscriberInterface()->publishNamespace(std::move(ann), nullptr);
    auto res = folly::coro::blockingWait(std::move(task), exec_.get());
    EXPECT_TRUE(res.hasValue());
    if (res.hasValue()) {
      if (addToState) {
        getOrCreateMockState(session)->publishNamespaceHandles.push_back(*res);
      }
      return *res;
    }
    return std::shared_ptr<Subscriber::PublishNamespaceHandle>(nullptr);
  });
}

std::shared_ptr<TrackConsumer> MoQRelayTest::doPublish(
    std::shared_ptr<MoQSession> session,
    const FullTrackName& trackName,
    bool addToState
) {
  PublishRequest pub;
  pub.fullTrackName = trackName;
  return withSessionContext(session, [&]() {
    auto res = subscriberInterface()->publish(std::move(pub), createMockSubscriptionHandle());
    EXPECT_TRUE(res.hasValue());
    if (res.hasValue()) {
      if (addToState) {
        getOrCreateMockState(session)->publishConsumers.push_back(res->consumer);
      }
      co_withExecutor(static_cast<folly::DrivableExecutor*>(exec_.get()), std::move(res->reply))
          .start();
      driveIfMultiThread(); // flush reply to relay exec so publish state is ready
      return res->consumer;
    }
    return std::shared_ptr<TrackConsumer>(nullptr);
  });
}

std::shared_ptr<Publisher::SubscribeNamespaceHandle> MoQRelayTest::doSubscribeNamespace(
    std::shared_ptr<MoQSession> session,
    const TrackNamespace& nsPrefix,
    bool addToState
) {
  SubscribeNamespace subNs;
  subNs.trackNamespacePrefix = nsPrefix;
  return withSessionContext(session, [&]() {
    auto task = publisherInterface()->subscribeNamespace(std::move(subNs), nullptr);
    auto res = folly::coro::blockingWait(std::move(task), exec_.get());
    EXPECT_TRUE(res.hasValue());
    if (res.hasValue()) {
      if (addToState) {
        getOrCreateMockState(session)->subscribeNamespaceHandles.push_back(*res);
      }
      return *res;
    }
    return std::shared_ptr<Publisher::SubscribeNamespaceHandle>(nullptr);
  });
}

std::shared_ptr<MockSubgroupConsumer> MoQRelayTest::createMockSubgroupConsumer() {
  auto sg = std::make_shared<NiceMock<MockSubgroupConsumer>>();
  ON_CALL(*sg, beginObject(_, _, _, _))
      .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
  ON_CALL(*sg, objectPayload(_, _))
      .WillByDefault(Return(folly::makeExpected<MoQPublishError>(ObjectPublishStatus::IN_PROGRESS))
      );
  ON_CALL(*sg, endOfSubgroup())
      .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
  ON_CALL(*sg, endOfGroup(_))
      .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
  ON_CALL(*sg, endOfTrackAndGroup(_))
      .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
  return sg;
}

std::shared_ptr<TrackConsumer> MoQRelayTest::doPublishWithHandle(
    std::shared_ptr<MoQSession> session,
    const FullTrackName& trackName,
    std::shared_ptr<Publisher::SubscriptionHandle> handle
) {
  return withSessionContext(session, [&]() -> std::shared_ptr<TrackConsumer> {
    PublishRequest pub;
    pub.fullTrackName = trackName;
    auto res = subscriberInterface()->publish(std::move(pub), std::move(handle));
    EXPECT_TRUE(res.hasValue());
    if (!res.hasValue()) {
      return nullptr;
    }
    auto consumer = res->consumer;
    getOrCreateMockState(session)->publishConsumers.push_back(consumer);
    co_withExecutor(static_cast<folly::DrivableExecutor*>(exec_.get()), std::move(res->reply))
        .start();
    driveIfMultiThread(); // flush reply to relay exec so publish state is ready
    return consumer;
  });
}

std::shared_ptr<Publisher::SubscribeNamespaceHandle> MoQRelayTest::doSubscribeNamespaceWithForward(
    std::shared_ptr<MoQSession> session,
    const TrackNamespace& nsPrefix,
    bool forward
) {
  SubscribeNamespace subNs;
  subNs.trackNamespacePrefix = nsPrefix;
  subNs.forward = forward;
  return withSessionContext(session, [&]() {
    auto task = publisherInterface()->subscribeNamespace(std::move(subNs), nullptr);
    auto res = folly::coro::blockingWait(std::move(task), exec_.get());
    EXPECT_TRUE(res.hasValue());
    if (!res.hasValue()) {
      return std::shared_ptr<Publisher::SubscribeNamespaceHandle>(nullptr);
    }
    getOrCreateMockState(session)->subscribeNamespaceHandles.push_back(*res);
    return *res;
  });
}

void MoQRelayTest::setupPublishSucceeds(std::shared_ptr<MockMoQSession> session) {
  ON_CALL(*session, publish(_, _))
      .WillByDefault(Invoke([this](PublishRequest pub, auto) -> Subscriber::PublishResult {
        PublishOk ok{
            pub.requestID,
            /*forward=*/pub.forward,
            /*priority=*/128,
            GroupOrder::Default,
            LocationType::LargestObject,
            /*start=*/std::nullopt,
            /*endGroup=*/std::make_optional(uint64_t(0))
        };
        return Subscriber::PublishConsumerAndReplyTask{
            createMockConsumer(),
            folly::coro::makeTask<folly::Expected<PublishOk, PublishError>>(std::move(ok))
        };
      }));
}

std::shared_ptr<NiceMock<MockSubscriptionHandle>> MoQRelayTest::makePublishHandle() {
  SubscribeOk ok;
  ok.requestID = RequestID(0);
  ok.trackAlias = TrackAlias(0);
  ok.expires = std::chrono::milliseconds(0);
  ok.groupOrder = GroupOrder::Default;
  auto handle = std::make_shared<NiceMock<MockSubscriptionHandle>>(std::move(ok));
  ON_CALL(*handle, requestUpdateResult())
      .WillByDefault(Return(folly::makeExpected<RequestError>(RequestOk{})));
  return handle;
}

} // namespace moxygen::test
