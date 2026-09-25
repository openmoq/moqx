/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Originally from github.com/facebookexperimental/moxygen.
 * See the moxygen LICENSE for the original license terms:
 * https://github.com/openmoq/moxygen/blob/main/LICENSE
 *
 * Copyright (c) OpenMOQ contributors.
 */

#include "MoqxRelayTestFixture.h"

namespace openmoq::moqx::test {

class RecordingNamespacePublishHandle : public Publisher::NamespacePublishHandle {
public:
  void namespaceMsg(const Namespace& ns) override { namespaces.push_back(ns); }

  void namespaceMsg(const TrackNamespace& suffix) override {
    Namespace ns;
    ns.trackNamespaceSuffix = suffix;
    namespaces.push_back(std::move(ns));
  }

  void namespaceDoneMsg(const TrackNamespace& suffix) override { dones.push_back(suffix); }

  std::vector<Namespace> namespaces;
  std::vector<TrackNamespace> dones;
};

TEST_P(MoQRelayTest, LegacyPublisherSynthesizesStableOriginAndAppendsRelayHop) {
  constexpr uint64_t kRelayHop = 900;
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", kRelayHop);

  auto publisher = createMockSession();
  auto subscriber = createMockSession();
  ON_CALL(*subscriber, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft16)));
  ON_CALL(*subscriber, negotiatedSetupExtension(SetupExtension::RelayHops))
      .WillByDefault(Return(true));
  auto namespaceHandle = std::make_shared<RecordingNamespacePublishHandle>();
  doSubscribeNamespace(subscriber, kTestNamespace, true, namespaceHandle);

  doPublishNamespace(publisher, kTestNamespace);
  driveIfMultiThread();

  ASSERT_EQ(namespaceHandle->namespaces.size(), 1);
  ASSERT_EQ(namespaceHandle->namespaces.front().params.size(), 1);
  auto path = decodeRelayHopPath(
      namespaceHandle->namespaces.front().params.at(0).asString,
      kVersionDraft16
  );
  ASSERT_TRUE(path.hasValue());
  ASSERT_EQ(path->size(), 2);
  EXPECT_GT(path->front(), 0);
  EXPECT_LE(path->front(), kMaxRelayHopID);
  EXPECT_EQ(path->back(), kRelayHop);

  doPublishNamespace(publisher, TrackNamespace{{"test", "namespace", "child"}});
  driveIfMultiThread();
  ASSERT_EQ(namespaceHandle->namespaces.size(), 2);
  auto secondPath =
      decodeRelayHopPath(namespaceHandle->namespaces.back().params.at(0).asString, kVersionDraft16);
  ASSERT_TRUE(secondPath.hasValue());
  EXPECT_EQ(secondPath.value(), path.value());

  removeSession(publisher);
  removeSession(subscriber);
  driveIfMultiThread();
}

TEST_P(MoQRelayTest, LegacyPublishersReceiveDistinctOriginHopIDs) {
  constexpr uint64_t kRelayHop = 900;
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", kRelayHop);

  auto subscriber = createMockSession();
  ON_CALL(*subscriber, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft16)));
  ON_CALL(*subscriber, negotiatedSetupExtension(SetupExtension::RelayHops))
      .WillByDefault(Return(true));
  auto namespaceHandle = std::make_shared<RecordingNamespacePublishHandle>();
  doSubscribeNamespace(subscriber, TrackNamespace{}, true, namespaceHandle);

  auto publisherA = createMockSession();
  auto publisherB = createMockSession();
  doPublishNamespace(publisherA, TrackNamespace{{"publisher-a"}});
  doPublishNamespace(publisherB, TrackNamespace{{"publisher-b"}});
  driveIfMultiThread();

  ASSERT_EQ(namespaceHandle->namespaces.size(), 2);
  auto pathA =
      decodeRelayHopPath(namespaceHandle->namespaces[0].params.at(0).asString, kVersionDraft16);
  auto pathB =
      decodeRelayHopPath(namespaceHandle->namespaces[1].params.at(0).asString, kVersionDraft16);
  ASSERT_TRUE(pathA.hasValue());
  ASSERT_TRUE(pathB.hasValue());
  ASSERT_EQ(pathA->size(), 2);
  ASSERT_EQ(pathB->size(), 2);
  EXPECT_NE(pathA->front(), pathB->front());

  removeSession(publisherA);
  removeSession(publisherB);
  removeSession(subscriber);
  driveIfMultiThread();
}

TEST_P(MoQRelayTest, RelayHopLoopIsDroppedBeforeNamespaceRegistration) {
  constexpr uint64_t kRelayHop = 900;
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", kRelayHop);
  auto publisher = createMockSession();
  ON_CALL(*publisher, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft16)));
  ON_CALL(*publisher, negotiatedSetupExtension(SetupExtension::RelayHops))
      .WillByDefault(Return(true));

  PublishNamespace pubNs;
  pubNs.trackNamespace = kTestNamespace;
  pubNs.params.insertParam(Parameter(
      folly::to_underlying(TrackRequestParamKey::HOP_PATH),
      encodeRelayHopPath({100, kRelayHop, 200}, kVersionDraft16).value()
  ));
  auto result = withSessionContext(publisher, [&] {
    return folly::coro::blockingWait(
        subscriberInterface()->publishNamespace(std::move(pubNs), nullptr),
        exec_.get()
    );
  });
  EXPECT_TRUE(result.hasError());

  verifyOnRelayExec([&] { EXPECT_EQ(relay_->findPublishNamespaceSession(kTestNamespace), nullptr); }
  );
  removeSession(publisher);
}

TEST_P(MoQRelayTest, NegotiatedPublisherWithoutHopPathIsDropped) {
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", 900);
  auto publisher = createMockSession();
  ON_CALL(*publisher, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft16)));
  ON_CALL(*publisher, negotiatedSetupExtension(SetupExtension::RelayHops))
      .WillByDefault(Return(true));

  PublishNamespace pubNs;
  pubNs.trackNamespace = kTestNamespace;
  auto result = withSessionContext(publisher, [&] {
    return folly::coro::blockingWait(
        subscriberInterface()->publishNamespace(std::move(pubNs), nullptr),
        exec_.get()
    );
  });
  EXPECT_TRUE(result.hasError());
  EXPECT_FALSE(publisher->isClosed());

  verifyOnRelayExec([&] { EXPECT_EQ(relay_->findPublishNamespaceSession(kTestNamespace), nullptr); }
  );
  removeSession(publisher);
}

TEST_P(MoQRelayTest, MalformedRelayHopPathClosesSourceSession) {
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", 900);
  auto publisher = createMockSession();
  ON_CALL(*publisher, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft16)));
  ON_CALL(*publisher, negotiatedSetupExtension(SetupExtension::RelayHops))
      .WillByDefault(Return(true));

  PublishNamespace pubNs;
  pubNs.trackNamespace = kTestNamespace;
  pubNs.params.insertParam(
      Parameter(folly::to_underlying(TrackRequestParamKey::HOP_PATH), std::string("\x40", 1))
  );
  auto result = withSessionContext(publisher, [&] {
    return folly::coro::blockingWait(
        subscriberInterface()->publishNamespace(std::move(pubNs), nullptr),
        exec_.get()
    );
  });
  EXPECT_TRUE(result.hasError());
  driveIfMultiThread();
  EXPECT_TRUE(publisher->isClosed());

  verifyOnRelayExec([&] { EXPECT_EQ(relay_->findPublishNamespaceSession(kTestNamespace), nullptr); }
  );
  removeSession(publisher);
}

TEST_P(MoQRelayTest, ExcludeHopSuppressesOriginIntermediateAndLocalMatches) {
  constexpr uint64_t kRelayHop = 900;
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", kRelayHop);
  auto publisher = createMockSession();
  ON_CALL(*publisher, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft16)));
  ON_CALL(*publisher, negotiatedSetupExtension(SetupExtension::RelayHops))
      .WillByDefault(Return(true));

  std::vector<std::shared_ptr<MockMoQSession>> subscribers;
  std::vector<std::shared_ptr<RecordingNamespacePublishHandle>> namespaceHandles;
  std::vector<std::shared_ptr<Publisher::SubscribeNamespaceHandle>> subscribeHandles;
  for (uint64_t excludedHop : {uint64_t{100}, uint64_t{200}, kRelayHop}) {
    auto subscriber = createMockSession();
    ON_CALL(*subscriber, getNegotiatedVersion())
        .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft16)));
    ON_CALL(*subscriber, negotiatedSetupExtension(SetupExtension::RelayHops))
        .WillByDefault(Return(true));
    auto namespaceHandle = std::make_shared<RecordingNamespacePublishHandle>();
    SubscribeNamespace subNs;
    subNs.trackNamespacePrefix = kTestNamespace;
    subNs.params.insertParam(
        Parameter(folly::to_underlying(TrackRequestParamKey::EXCLUDE_HOP), excludedHop)
    );
    auto result = withSessionContext(subscriber, [&] {
      return folly::coro::blockingWait(
          publisherInterface()->subscribeNamespace(std::move(subNs), namespaceHandle),
          exec_.get()
      );
    });
    ASSERT_TRUE(result.hasValue());
    subscribeHandles.push_back(result.value());
    subscribers.push_back(std::move(subscriber));
    namespaceHandles.push_back(std::move(namespaceHandle));
  }

  PublishNamespace pubNs;
  pubNs.trackNamespace = kTestNamespace;
  pubNs.params.insertParam(Parameter(
      folly::to_underlying(TrackRequestParamKey::HOP_PATH),
      encodeRelayHopPath({100, 200}, kVersionDraft16).value()
  ));
  auto result = withSessionContext(publisher, [&] {
    return folly::coro::blockingWait(
        subscriberInterface()->publishNamespace(std::move(pubNs), nullptr),
        exec_.get()
    );
  });
  ASSERT_TRUE(result.hasValue());
  driveIfMultiThread();

  for (const auto& namespaceHandle : namespaceHandles) {
    EXPECT_TRUE(namespaceHandle->namespaces.empty());
  }

  removeSession(publisher);
  for (auto& subscriber : subscribers) {
    removeSession(subscriber);
  }
  subscribeHandles.clear();
  driveIfMultiThread();
}

TEST_P(MoQRelayTest, NonNegotiatedSubscriberReceivesLegacyNamespaceMessage) {
  constexpr uint64_t kRelayHop = 900;
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", kRelayHop);
  auto publisher = createMockSession();
  ON_CALL(*publisher, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft16)));
  ON_CALL(*publisher, negotiatedSetupExtension(SetupExtension::RelayHops))
      .WillByDefault(Return(true));
  auto subscriber = createMockSession();
  ON_CALL(*subscriber, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft16)));
  auto namespaceHandle = std::make_shared<RecordingNamespacePublishHandle>();
  doSubscribeNamespace(subscriber, kTestNamespace, true, namespaceHandle);

  PublishNamespace pubNs;
  pubNs.trackNamespace = kTestNamespace;
  pubNs.params.insertParam(Parameter(
      folly::to_underlying(TrackRequestParamKey::HOP_PATH),
      encodeRelayHopPath({100, 200}, kVersionDraft16).value()
  ));
  auto result = withSessionContext(publisher, [&] {
    return folly::coro::blockingWait(
        subscriberInterface()->publishNamespace(std::move(pubNs), nullptr),
        exec_.get()
    );
  });
  ASSERT_TRUE(result.hasValue());
  driveIfMultiThread();

  ASSERT_EQ(namespaceHandle->namespaces.size(), 1);
  EXPECT_TRUE(namespaceHandle->namespaces.front().params.empty());

  removeSession(publisher);
  removeSession(subscriber);
  driveIfMultiThread();
}

TEST_P(MoQRelayTest, ExcludedSubscriberGetsNoNamespaceDone) {
  constexpr uint64_t kRelayHop = 900;
  constexpr uint64_t kOriginHop = 100;
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", kRelayHop);

  auto publisher = createMockSession();
  ON_CALL(*publisher, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft16)));
  ON_CALL(*publisher, negotiatedSetupExtension(SetupExtension::RelayHops))
      .WillByDefault(Return(true));

  std::vector<std::shared_ptr<MockMoQSession>> subscribers;
  std::vector<std::shared_ptr<RecordingNamespacePublishHandle>> namespaceHandles;
  std::vector<std::shared_ptr<Publisher::SubscribeNamespaceHandle>> subscribeHandles;
  for (bool exclude : {true, false}) {
    auto subscriber = createMockSession();
    ON_CALL(*subscriber, getNegotiatedVersion())
        .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft16)));
    ON_CALL(*subscriber, negotiatedSetupExtension(SetupExtension::RelayHops))
        .WillByDefault(Return(true));
    auto namespaceHandle = std::make_shared<RecordingNamespacePublishHandle>();
    SubscribeNamespace subNs;
    subNs.trackNamespacePrefix = kTestNamespace;
    if (exclude) {
      subNs.params.insertParam(
          Parameter(folly::to_underlying(TrackRequestParamKey::EXCLUDE_HOP), kOriginHop)
      );
    }
    auto result = withSessionContext(subscriber, [&] {
      return folly::coro::blockingWait(
          publisherInterface()->subscribeNamespace(std::move(subNs), namespaceHandle),
          exec_.get()
      );
    });
    ASSERT_TRUE(result.hasValue());
    subscribeHandles.push_back(result.value());
    subscribers.push_back(std::move(subscriber));
    namespaceHandles.push_back(std::move(namespaceHandle));
  }

  PublishNamespace pubNs;
  pubNs.trackNamespace = kTestNamespace;
  pubNs.params.insertParam(Parameter(
      folly::to_underlying(TrackRequestParamKey::HOP_PATH),
      encodeRelayHopPath({kOriginHop}, kVersionDraft16).value()
  ));
  auto result = withSessionContext(publisher, [&] {
    return folly::coro::blockingWait(
        subscriberInterface()->publishNamespace(std::move(pubNs), nullptr),
        exec_.get()
    );
  });
  ASSERT_TRUE(result.hasValue());
  driveIfMultiThread();

  EXPECT_TRUE(namespaceHandles[0]->namespaces.empty());
  EXPECT_EQ(namespaceHandles[1]->namespaces.size(), 1);

  verifyOnRelayExec([&] { relay_->doPublishNamespaceDone(kTestNamespace, publisher); });
  driveIfMultiThread();

  EXPECT_TRUE(namespaceHandles[0]->dones.empty());
  EXPECT_EQ(namespaceHandles[1]->dones.size(), 1);

  removeSession(publisher);
  for (auto& subscriber : subscribers) {
    removeSession(subscriber);
  }
  subscribeHandles.clear();
  driveIfMultiThread();
}

TEST_P(MoQRelayTest, SubscribeNamespaceDoesntAddDrainingPublish) {
  auto publisherSession = createMockSession();
  auto subscriber1 = createMockSession();
  auto subscriber2 = createMockSession();

  // Subscriber 1 subscribes to publishNamespaces
  auto handle1 = doSubscribeNamespace(subscriber1, kTestNamespace, /*addToState=*/false);

  // Publish first track - subscriber 1 should receive it
  auto mockConsumer1 = createMockConsumer();
  EXPECT_CALL(*subscriber1, publish(testing::_, testing::_))
      .WillOnce([mockConsumer1](const auto& /*pubReq*/, auto /*subHandle*/) {
        return Subscriber::PublishResult(Subscriber::PublishConsumerAndReplyTask{
            mockConsumer1,
            []() -> folly::coro::Task<folly::Expected<PublishOk, PublishError>> {
              co_return PublishOk{
                  /*requestID=*/RequestID(1),
                  /*forward=*/true,
                  /*subscriberPriority=*/0,
                  /*groupOrder=*/GroupOrder::OldestFirst,
                  /*locType=*/LocationType::LargestObject,
                  /*start=*/std::nullopt,
                  /*endGroup=*/std::nullopt
              };
            }()
        });
      });

  EXPECT_CALL(*mockConsumer1, beginSubgroup(_, _, _, _))
      .WillOnce([&](uint64_t, uint64_t, uint8_t, moxygen::BeginSubgroupOptions) {
        auto sg = std::make_shared<NiceMock<MockSubgroupConsumer>>();
        EXPECT_CALL(*sg, endOfSubgroup()).WillOnce(testing::Return(folly::unit));
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(sg);
      });

  // Begin a subgroup for ongoing publish activity
  auto pubConsumer = doPublish(
      publisherSession,
      FullTrackName{kTestNamespace, "track_stream"},
      /*addToState=*/false
  );
  // TODO: bug subscriber not added until next loop?
  exec_->drive();
  auto subgroupRes = pubConsumer->beginSubgroup(0, 0, 0);
  EXPECT_TRUE(subgroupRes.hasValue());
  auto subgroup = *subgroupRes;

  // publisher ends subscription
  EXPECT_CALL(*mockConsumer1, publishDone(testing::_));
  EXPECT_TRUE(
      pubConsumer->publishDone({RequestID(1), PublishDoneStatusCode::TRACK_ENDED, 0, "track ended"})
          .hasValue()
  );
  subgroup->endOfSubgroup();

  // Subscriber 2 subscribes to publishNamespaces but doesn't get finished track
  doSubscribeNamespace(subscriber2, kTestNamespace);

  // First publish (existing context handles initial publish), now publish a
  // second track
  // Expect publish calls on both subscribers, just fail them.
  EXPECT_CALL(*subscriber1, publish(testing::_, testing::_))
      .WillOnce([](const auto& /*pubReq*/, auto /*subHandle*/) {
        return folly::makeUnexpected(PublishError{});
      });

  EXPECT_CALL(*subscriber2, publish(testing::_, testing::_))
      .WillOnce([](const auto& /*pubReq*/, auto /*subHandle*/) {
        return folly::makeUnexpected(PublishError{});
      });

  auto pubConsumer2 = doPublish(publisherSession, FullTrackName{kTestNamespace, "track_stream_2"});
  exec_->drive();

  removeSession(publisherSession);
  removeSession(subscriber1);
  removeSession(subscriber2);
  driveIfMultiThread(); // flush relay cleanup so it drops session refs before mocks are destroyed
}

TEST_P(MoQRelayTest, SubscribeNamespaceEmptyPrefixAllowed) {
  auto session = createMockSession();
  auto publisher = createMockSession();

  auto handle = std::make_shared<NiceMock<MockNamespacePublishHandle>>();
  EXPECT_CALL(*handle, namespaceMsg(_)).Times(1);
  EXPECT_CALL(*session, publishNamespace(_, _)).Times(0);

  TrackNamespace emptyNs{{}};
  doSubscribeNamespace(session, emptyNs, /*addToState=*/true, handle);
  doPublishNamespace(publisher, kTestNamespace);
  exec_->drive();

  removeSession(publisher);
  removeSession(session);
  driveIfMultiThread();
}

TEST_P(MoQRelayTest, ExactNamespaceSubscriberReceivesNamespace) {
  auto subscriber = createMockSession();
  auto publisher = createMockSession();

  // An exact-match subscriber gets a bidi NAMESPACE with an empty suffix, never
  // a separate-stream PUBLISH_NAMESPACE.
  auto handle = std::make_shared<NiceMock<MockNamespacePublishHandle>>();
  EXPECT_CALL(*handle, namespaceMsg(_)).WillOnce([](const TrackNamespace& suffix) {
    EXPECT_TRUE(suffix.empty());
  });
  EXPECT_CALL(*subscriber, publishNamespace(_, _)).Times(0);
  doSubscribeNamespace(subscriber, kTestNamespace, /*addToState=*/true, handle);

  doPublishNamespace(publisher, kTestNamespace);
  exec_->drive();

  removeSession(publisher);
  removeSession(subscriber);
  driveIfMultiThread(); // flush exec-hopped handle destruction before mocks are destroyed
}

// Bug: when a subscriber with forward=true joins a namespace whose track
// forwarder is empty, the relay fires REQUEST_UPDATE twice — once explicitly
// at the if(forwarder->empty()) site and once via forwardChanged() when
// addSubscriber() increments numForwardingSubscribers from 0 to 1.
TEST_P(MoQRelayTest, SubscribeNs_ForwardTrue_EmptyForwarder_SingleRequestUpdate) {
  auto pubSession = createMockSession();
  doPublishNamespace(pubSession, kTestNamespace);
  auto mockHandle = makePublishHandle();
  doPublishWithHandle(pubSession, kTestTrackName, mockHandle);

  // Expect exactly one REQUEST_UPDATE(forward=true).
  // Before the fix this fires twice.
  EXPECT_CALL(*mockHandle, requestUpdateCalled(_)).Times(1).WillOnce([](const RequestUpdate& u) {
    ASSERT_TRUE(u.forward.has_value());
    EXPECT_TRUE(*u.forward);
  });

  auto subSession = createMockSession();
  setupPublishSucceeds(subSession);
  doSubscribeNamespaceWithForward(subSession, kTestNamespace, /*forward=*/true);

  for (int i = 0; i < 5; i++) {
    exec_->drive();
  }

  // Verify before cleanup — cleanup itself legitimately sends forward=false
  // when the subscriber leaves and the forwarder drains.
  ASSERT_TRUE(testing::Mock::VerifyAndClearExpectations(mockHandle.get()));

  removeSession(subSession);
  removeSession(pubSession);
  for (int i = 0; i < 3; i++) {
    exec_->drive();
  }
}

// Bug: when a subscriber with forward=false joins a namespace whose track
// forwarder is empty, the relay fires a spurious REQUEST_UPDATE(forward=false)
// at the if(forwarder->empty()) site — even though the upstream is already at
// forward=false (set by publish() which found no subscribers).
TEST_P(MoQRelayTest, SubscribeNs_ForwardFalse_EmptyForwarder_NoRequestUpdate) {
  auto pubSession = createMockSession();
  doPublishNamespace(pubSession, kTestNamespace);
  auto mockHandle = makePublishHandle();
  doPublishWithHandle(pubSession, kTestTrackName, mockHandle);

  // Expect no REQUEST_UPDATE at all.
  // Before the fix this fires once with forward=false.
  EXPECT_CALL(*mockHandle, requestUpdateCalled(_)).Times(0);

  auto subSession = createMockSession();
  setupPublishSucceeds(subSession);
  doSubscribeNamespaceWithForward(subSession, kTestNamespace, /*forward=*/false);

  for (int i = 0; i < 5; i++) {
    exec_->drive();
  }

  ASSERT_TRUE(testing::Mock::VerifyAndClearExpectations(mockHandle.get()));

  removeSession(subSession);
  removeSession(pubSession);
  for (int i = 0; i < 3; i++) {
    exec_->drive();
  }
}

// Bug: publishWithSession counts every matching namespace subscriber, ignoring its
// forward flag, so a track whose only subscriber has forward=false is answered with
// PUBLISH_OK(forward=true). The publisher then sends objects the forwarder has no
// forwarding subscriber to deliver to, and no forwardChanged fires to correct it
// because forwardingSubscribers_ never leaves 0.
TEST_P(MoQRelayTest, PublishForwardFalseWhenOnlySubscriberIsNotForwarding) {
  auto subSession = createMockSession();
  setupPublishSucceeds(subSession);
  doSubscribeNamespaceWithForward(subSession, kTestNamespace, /*forward=*/false);

  auto pubSession = createMockSession();
  doPublishNamespace(pubSession, kTestNamespace);

  auto mockHandle = makePublishHandle();
  withSessionContext(pubSession, [&]() {
    PublishRequest pub;
    pub.fullTrackName = kTestTrackName;
    auto res = subscriberInterface()->publish(std::move(pub), mockHandle);
    ASSERT_TRUE(res.hasValue());
    getOrCreateMockState(pubSession)->publishConsumers.push_back(res->consumer);
    auto reply = folly::coro::blockingWait(std::move(res->reply), exec_.get());
    ASSERT_TRUE(reply.hasValue());
    EXPECT_FALSE(reply->forward
    ) << "no subscriber is forwarding, so the relay must not ask for objects";
  });

  removeSession(subSession);
  removeSession(pubSession);
  for (int i = 0; i < 3; i++) {
    exec_->drive();
  }
}

} // namespace openmoq::moqx::test
