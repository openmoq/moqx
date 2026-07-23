/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Originally from github.com/facebookexperimental/moxygen.
 * See deps/moxygen/LICENSE for the original license terms.
 *
 * Copyright (c) OpenMOQ contributors.
 */

#include "MoqxRelayTestFixture.h"

namespace moxygen::test {

class RecordingNamespacePublishHandle : public Publisher::NamespacePublishHandle {
public:
  void namespaceMsg(const Namespace& ns) override { namespaces.push_back(ns); }

  void namespaceMsg(const TrackNamespace& suffix) override {
    Namespace ns;
    ns.trackNamespaceSuffix = suffix;
    namespaces.push_back(std::move(ns));
  }

  void namespaceDoneMsg(const TrackNamespace&) override {}

  std::vector<Namespace> namespaces;
};

TEST_P(MoQRelayTest, LegacyPublisherSynthesizesStableOriginAndAppendsRelayHop) {
  constexpr uint64_t kSourceHop = 101;
  constexpr uint64_t kRelayHop = 900;
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", kRelayHop);

  auto publisher = createMockSession();
  ON_CALL(*publisher, getRelayHopSourceID()).WillByDefault(Return(kSourceHop));
  auto subscriber = createMockSession();
  ON_CALL(*subscriber, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft16)));
  ON_CALL(*subscriber, isRelayHopsNegotiated()).WillByDefault(Return(true));
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
  EXPECT_EQ(path.value(), (std::vector<uint64_t>{kSourceHop, kRelayHop}));

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

TEST_P(MoQRelayTest, RelayHopLoopIsDroppedBeforeNamespaceRegistration) {
  constexpr uint64_t kRelayHop = 900;
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", kRelayHop);
  auto publisher = createMockSession();
  ON_CALL(*publisher, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft16)));
  ON_CALL(*publisher, isRelayHopsNegotiated()).WillByDefault(Return(true));

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
  ON_CALL(*publisher, isRelayHopsNegotiated()).WillByDefault(Return(true));

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
  ON_CALL(*publisher, isRelayHopsNegotiated()).WillByDefault(Return(true));

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
  ON_CALL(*publisher, isRelayHopsNegotiated()).WillByDefault(Return(true));

  std::vector<std::shared_ptr<MockMoQSession>> subscribers;
  std::vector<std::shared_ptr<RecordingNamespacePublishHandle>> namespaceHandles;
  std::vector<std::shared_ptr<Publisher::SubscribeNamespaceHandle>> subscribeHandles;
  for (uint64_t excludedHop : {uint64_t{100}, uint64_t{200}, kRelayHop}) {
    auto subscriber = createMockSession();
    ON_CALL(*subscriber, getNegotiatedVersion())
        .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft16)));
    ON_CALL(*subscriber, isRelayHopsNegotiated()).WillByDefault(Return(true));
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
  ON_CALL(*publisher, isRelayHopsNegotiated()).WillByDefault(Return(true));
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

TEST_P(MoQRelayTest, SubscribeNamespaceEmptyPrefixRejectedPreV16) {
  // Default session uses kVersionDraftCurrent (draft-14, which is < 16)
  auto session = createMockSession();

  TrackNamespace emptyNs{{}};
  SubscribeNamespace subNs;
  subNs.trackNamespacePrefix = emptyNs;

  withSessionContext(session, [&]() {
    auto task = publisherInterface()->subscribeNamespace(std::move(subNs), nullptr);
    auto res = folly::coro::blockingWait(std::move(task), exec_.get());
    ASSERT_FALSE(res.hasValue()
    ) << "Empty namespace prefix should be rejected for pre-v16 sessions";
    EXPECT_EQ(res.error().errorCode, SubscribeNamespaceErrorCode::NAMESPACE_PREFIX_UNKNOWN);
    EXPECT_EQ(res.error().reasonPhrase, "empty");
  });

  removeSession(session);
}

TEST_P(MoQRelayTest, SubscribeNamespaceEmptyPrefixAllowedV16) {
  auto session = createMockSession();
  // Override the negotiated version to draft-16
  ON_CALL(*session, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft16)));
  auto publisher = createMockSession();

  // A draft-16 subscriber receives announcements via the bidi NAMESPACE message
  // (on the publish handle), not the separate-stream PUBLISH_NAMESPACE path.
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

TEST_P(MoQRelayTest, ExactNamespaceSubscriberReceivesPublishNamespace) {
  auto subscriber = createMockSession(); // default kVersionDraftCurrent (< 16)
  auto publisher = createMockSession();

  // Subscribe with a non-null publish handle, as MoQRelaySession always does.
  // A draft <= 15 subscriber must forward via the separate-stream
  // PUBLISH_NAMESPACE path, never the bidi handle (whose namespaceMsg is an
  // unimplemented moxygen stub that aborts the relay).
  auto handle = std::make_shared<NiceMock<MockNamespacePublishHandle>>();
  EXPECT_CALL(*handle, namespaceMsg(_)).Times(0);
  doSubscribeNamespace(subscriber, kTestNamespace, /*addToState=*/true, handle);

  // Expect the subscriber to receive a publishNamespace forwarding when
  // the publisher announces the same exact namespace
  EXPECT_CALL(*subscriber, publishNamespace(_, _))
      .WillOnce(
          [](PublishNamespace ann, auto) -> folly::coro::Task<Subscriber::PublishNamespaceResult> {
            EXPECT_EQ(ann.trackNamespace, kTestNamespace);
            co_return folly::makeUnexpected(PublishNamespaceError{
                ann.requestID,
                PublishNamespaceErrorCode::UNINTERESTED,
                "test"
            });
          }
      );

  // Publisher announces the same exact namespace
  doPublishNamespace(publisher, kTestNamespace);

  // Drive the executor so the async publishNamespace forwarding runs
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

} // namespace moxygen::test
