/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Copyright (c) OpenMOQ contributors.
 * Originally from github.com/facebookexperimental/moxygen.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
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

// A draft-18 session that negotiated MoQ Cluster and declared peerHop.
void negotiateCluster(MockMoQSession& session, uint64_t peerHop = 0) {
  ON_CALL(session, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft18)));
  ON_CALL(session, negotiatedSetupExtension(SetupExtension::RelayHops)).WillByDefault(Return(true));
  ON_CALL(session, getPeerHopID()).WillByDefault(Return(peerHop));
}

// The relay advertises a negotiated sender's ROUTE_COST plus the sender's link cost,
// saturating. A peer that subscribes later is not replayed a path through it.
TEST_P(MoQRelayTest, RouteCostAccumulatesAndReplaySkipsPeersOnThePath) {
  constexpr uint64_t kRelayHop = 900;
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", kRelayHop);
  auto publisher = createMockSession();
  negotiateCluster(*publisher);
  ON_CALL(*publisher, getRelayLinkCost()).WillByDefault(Return(uint64_t{3}));
  auto publish = [&](TrackNamespace ns, uint64_t cost) {
    PublishNamespace pubNs;
    pubNs.trackNamespace = std::move(ns);
    pubNs.params.insertParam(Parameter(
        folly::to_underlying(TrackRequestParamKey::HOP_PATH),
        encodeRelayHopPath({100, 200}, kVersionDraft18).value()
    ));
    pubNs.params.insertParam(Parameter(folly::to_underlying(TrackRequestParamKey::ROUTE_COST), cost)
    );
    auto result = withSessionContext(publisher, [&] {
      return folly::coro::blockingWait(
          subscriberInterface()->publishNamespace(std::move(pubNs), nullptr),
          exec_.get()
      );
    });
    ASSERT_TRUE(result.hasValue());
    driveIfMultiThread();
  };
  publish(kTestNamespace, 5);
  publish(TrackNamespace{{"test", "saturated"}}, std::numeric_limits<uint64_t>::max());

  const TrackNamespace prefix{{"test"}};
  std::vector<std::shared_ptr<MockMoQSession>> sessions{publisher};
  for (uint64_t peerHop : {uint64_t{100}, uint64_t{200}, kRelayHop}) {
    auto peer = createMockSession();
    negotiateCluster(*peer, peerHop);
    auto handle = std::make_shared<RecordingNamespacePublishHandle>();
    doSubscribeNamespace(peer, prefix, true, handle);
    EXPECT_TRUE(handle->namespaces.empty()) << "peer hop " << peerHop;
    sessions.push_back(std::move(peer));
  }

  auto other = createMockSession();
  negotiateCluster(*other, 300);
  auto handle = std::make_shared<RecordingNamespacePublishHandle>();
  doSubscribeNamespace(other, prefix, true, handle);
  sessions.push_back(other);
  ASSERT_EQ(handle->namespaces.size(), 2u);
  for (const auto& ns : handle->namespaces) {
    const auto* cost = ns.params.getFirstParam(TrackRequestParamKey::ROUTE_COST);
    ASSERT_NE(cost, nullptr);
    const bool saturated = ns.trackNamespaceSuffix == TrackNamespace{{"saturated"}};
    EXPECT_EQ(cost->asUint64, saturated ? std::numeric_limits<uint64_t>::max() : 8u);
  }

  for (const auto& session : sessions) {
    removeSession(session);
  }
  driveIfMultiThread();
}

// A second cluster advertisement of a namespace coexists with the first and leaves
// the subscriptions it serves running. A second advertisement from a session without MoQ
// Cluster replaces the first and ends them.
TEST_P(MoQRelayTest, ClusterRoutesCoexistButPlainRoutesReplace) {
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", 900);
  auto tracks = [&](const TrackNamespace& ns) {
    size_t matched = 0;
    verifyOnRelayExec([&] { matched = relay_->matchTracks(ns, nullptr, 10).matched; });
    return matched;
  };
  auto advertise = [&](const std::shared_ptr<MockMoQSession>& session, uint64_t origin) {
    PublishNamespace pubNs;
    pubNs.trackNamespace = kTestNamespace;
    pubNs.params.insertParam(Parameter(
        folly::to_underlying(TrackRequestParamKey::HOP_PATH),
        encodeRelayHopPath({origin}, kVersionDraft18).value()
    ));
    auto result = withSessionContext(session, [&] {
      return folly::coro::blockingWait(
          subscriberInterface()->publishNamespace(std::move(pubNs), nullptr),
          exec_.get()
      );
    });
    ASSERT_TRUE(result.hasValue());
    driveIfMultiThread();
  };

  auto first = createMockSession();
  negotiateCluster(*first);
  advertise(first, 100);
  doPublish(first, kTestTrackName);
  ASSERT_EQ(tracks(kTestNamespace), 1u);
  auto second = createMockSession();
  negotiateCluster(*second);
  advertise(second, 200);
  EXPECT_EQ(tracks(kTestNamespace), 1u);

  const TrackNamespace plainNs{{"test", "plain"}};
  auto plain1 = createMockSession();
  doPublishNamespace(plain1, plainNs);
  doPublish(plain1, FullTrackName{plainNs, "track1"});
  ASSERT_EQ(tracks(plainNs), 1u);
  auto plain2 = createMockSession();
  doPublishNamespace(plain2, plainNs);
  driveIfMultiThread();
  EXPECT_EQ(tracks(plainNs), 0u);
  EXPECT_EQ(tracks(kTestNamespace), 1u);

  for (const auto& session : {first, second, plain1, plain2}) {
    removeSession(session);
  }
  driveIfMultiThread();
}

TEST_P(MoQRelayTest, AnonymousPublisherUsesZeroOriginAndAppendsRelayHop) {
  constexpr uint64_t kRelayHop = 900;
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", kRelayHop);

  auto publisher = createMockSession();
  auto subscriber = createMockSession();
  negotiateCluster(*subscriber);
  auto namespaceHandle = std::make_shared<RecordingNamespacePublishHandle>();
  doSubscribeNamespace(subscriber, kTestNamespace, true, namespaceHandle);

  doPublishNamespace(publisher, kTestNamespace);
  driveIfMultiThread();

  ASSERT_EQ(namespaceHandle->namespaces.size(), 1);
  const auto& params = namespaceHandle->namespaces.front().params;
  ASSERT_EQ(params.size(), 2);
  auto path = decodeRelayHopPath(
      params.getFirstParam(TrackRequestParamKey::HOP_PATH)->asString,
      kVersionDraft18
  );
  ASSERT_TRUE(path.hasValue());
  ASSERT_EQ(path->size(), 2);
  EXPECT_EQ(path->front(), kMoQClusterAnonHopId);
  EXPECT_EQ(path->back(), kRelayHop);
  // The publisher did not declare RELAY_COST, so its link costs the default 1.
  EXPECT_EQ(params.getFirstParam(TrackRequestParamKey::ROUTE_COST)->asUint64, 1u);

  doPublishNamespace(publisher, TrackNamespace{{"test", "namespace", "child"}});
  driveIfMultiThread();
  ASSERT_EQ(namespaceHandle->namespaces.size(), 2);
  auto secondPath = decodeRelayHopPath(
      namespaceHandle->namespaces.back()
          .params.getFirstParam(TrackRequestParamKey::HOP_PATH)
          ->asString,
      kVersionDraft18
  );
  ASSERT_TRUE(secondPath.hasValue());
  EXPECT_EQ(secondPath.value(), path.value());

  removeSession(publisher);
  removeSession(subscriber);
  driveIfMultiThread();
}

TEST_P(MoQRelayTest, AnonymousPublishersUseReservedZeroHopID) {
  constexpr uint64_t kRelayHop = 900;
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", kRelayHop);

  auto subscriber = createMockSession();
  negotiateCluster(*subscriber);
  auto namespaceHandle = std::make_shared<RecordingNamespacePublishHandle>();
  doSubscribeNamespace(subscriber, TrackNamespace{}, true, namespaceHandle);

  auto publisherA = createMockSession();
  auto publisherB = createMockSession();
  doPublishNamespace(publisherA, TrackNamespace{{"publisher-a"}});
  doPublishNamespace(publisherB, TrackNamespace{{"publisher-b"}});
  driveIfMultiThread();

  ASSERT_EQ(namespaceHandle->namespaces.size(), 2);
  for (const auto& ns : namespaceHandle->namespaces) {
    auto path = decodeRelayHopPath(
        ns.params.getFirstParam(TrackRequestParamKey::HOP_PATH)->asString,
        kVersionDraft18
    );
    ASSERT_TRUE(path.hasValue());
    EXPECT_EQ(path.value(), (std::vector<uint64_t>{kMoQClusterAnonHopId, kRelayHop}));
  }

  removeSession(publisherA);
  removeSession(publisherB);
  removeSession(subscriber);
  driveIfMultiThread();
}

TEST_P(MoQRelayTest, RelayHopLoopIsDroppedBeforeNamespaceRegistration) {
  constexpr uint64_t kRelayHop = 900;
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", kRelayHop);
  auto publisher = createMockSession();
  negotiateCluster(*publisher);

  PublishNamespace pubNs;
  pubNs.trackNamespace = kTestNamespace;
  pubNs.params.insertParam(Parameter(
      folly::to_underlying(TrackRequestParamKey::HOP_PATH),
      encodeRelayHopPath({100, kRelayHop, 200}, kVersionDraft18).value()
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

TEST_P(MoQRelayTest, NegotiatedPublisherWithoutHopPathClosesSession) {
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", 900);
  auto publisher = createMockSession();
  negotiateCluster(*publisher);

  PublishNamespace pubNs;
  pubNs.trackNamespace = kTestNamespace;
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

TEST_P(MoQRelayTest, MalformedRelayHopPathClosesSourceSession) {
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", 900);
  auto publisher = createMockSession();
  negotiateCluster(*publisher);

  PublishNamespace pubNs;
  pubNs.trackNamespace = kTestNamespace;
  // A truncated two-byte draft-18 varint.
  pubNs.params.insertParam(
      Parameter(folly::to_underlying(TrackRequestParamKey::HOP_PATH), std::string("\x80", 1))
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

// A peer whose Hop ID is on the path, or is ours, must not be advertised the path.
TEST_P(MoQRelayTest, PeerHopIDSuppressesOriginIntermediateAndLocalMatches) {
  constexpr uint64_t kRelayHop = 900;
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", kRelayHop);
  auto publisher = createMockSession();
  negotiateCluster(*publisher);

  std::vector<std::shared_ptr<MockMoQSession>> subscribers;
  std::vector<std::shared_ptr<RecordingNamespacePublishHandle>> namespaceHandles;
  for (uint64_t peerHop : {uint64_t{100}, uint64_t{200}, kRelayHop}) {
    auto subscriber = createMockSession();
    negotiateCluster(*subscriber, peerHop);
    auto namespaceHandle = std::make_shared<RecordingNamespacePublishHandle>();
    doSubscribeNamespace(subscriber, kTestNamespace, true, namespaceHandle);
    subscribers.push_back(std::move(subscriber));
    namespaceHandles.push_back(std::move(namespaceHandle));
  }

  PublishNamespace pubNs;
  pubNs.trackNamespace = kTestNamespace;
  pubNs.params.insertParam(Parameter(
      folly::to_underlying(TrackRequestParamKey::HOP_PATH),
      encodeRelayHopPath({100, 200}, kVersionDraft18).value()
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
  driveIfMultiThread();
}

TEST_P(MoQRelayTest, NonNegotiatedSubscriberReceivesNamespaceWithoutHopPath) {
  constexpr uint64_t kRelayHop = 900;
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", kRelayHop);
  auto publisher = createMockSession();
  negotiateCluster(*publisher);
  auto subscriber = createMockSession();
  ON_CALL(*subscriber, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft16)));
  auto namespaceHandle = std::make_shared<RecordingNamespacePublishHandle>();
  doSubscribeNamespace(subscriber, kTestNamespace, true, namespaceHandle);

  PublishNamespace pubNs;
  pubNs.trackNamespace = kTestNamespace;
  pubNs.params.insertParam(Parameter(
      folly::to_underlying(TrackRequestParamKey::HOP_PATH),
      encodeRelayHopPath({100, 200}, kVersionDraft18).value()
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
  negotiateCluster(*publisher);

  std::vector<std::shared_ptr<MockMoQSession>> subscribers;
  std::vector<std::shared_ptr<RecordingNamespacePublishHandle>> namespaceHandles;
  for (bool exclude : {true, false}) {
    auto subscriber = createMockSession();
    negotiateCluster(*subscriber, exclude ? kOriginHop : 0);
    auto namespaceHandle = std::make_shared<RecordingNamespacePublishHandle>();
    doSubscribeNamespace(subscriber, kTestNamespace, true, namespaceHandle);
    subscribers.push_back(std::move(subscriber));
    namespaceHandles.push_back(std::move(namespaceHandle));
  }

  PublishNamespace pubNs;
  pubNs.trackNamespace = kTestNamespace;
  pubNs.params.insertParam(Parameter(
      folly::to_underlying(TrackRequestParamKey::HOP_PATH),
      encodeRelayHopPath({kOriginHop}, kVersionDraft18).value()
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
  driveIfMultiThread();
}

// Two cluster routes for one namespace coexist. A subscriber hears NAMESPACE_DONE
// only once no remaining route reaches it, and a late subscriber is replayed the
// best route.
TEST_P(MoQRelayTest, CoexistingRoutesKeepTheNamespaceUntilTheLastOneGoes) {
  constexpr uint64_t kRelayHop = 900;
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", kRelayHop);

  auto publishFrom = [&](const std::shared_ptr<MockMoQSession>& publisher,
                         std::vector<uint64_t> path,
                         uint64_t cost) {
    PublishNamespace pubNs;
    pubNs.trackNamespace = kTestNamespace;
    pubNs.params.insertParam(Parameter(
        folly::to_underlying(TrackRequestParamKey::HOP_PATH),
        encodeRelayHopPath(path, kVersionDraft18).value()
    ));
    pubNs.params.insertParam(Parameter(folly::to_underlying(TrackRequestParamKey::ROUTE_COST), cost)
    );
    auto result = withSessionContext(publisher, [&] {
      return folly::coro::blockingWait(
          subscriberInterface()->publishNamespace(std::move(pubNs), nullptr),
          exec_.get()
      );
    });
    ASSERT_TRUE(result.hasValue());
    driveIfMultiThread();
  };

  auto viaPeer = createMockSession();
  negotiateCluster(*viaPeer);
  auto direct = createMockSession();
  negotiateCluster(*direct);

  // open hears both routes; peer100 is excluded from the one through hop 100.
  auto open = createMockSession();
  negotiateCluster(*open);
  auto openHandle = std::make_shared<RecordingNamespacePublishHandle>();
  doSubscribeNamespace(open, kTestNamespace, true, openHandle);
  auto peer100 = createMockSession();
  negotiateCluster(*peer100, 100);
  auto peer100Handle = std::make_shared<RecordingNamespacePublishHandle>();
  doSubscribeNamespace(peer100, kTestNamespace, true, peer100Handle);

  publishFrom(viaPeer, {100}, 5);
  publishFrom(direct, {200}, 1);
  verifyOnRelayExec([&] {
    EXPECT_EQ(relay_->findPublishNamespaceSessions(kTestNamespace).size(), 2u);
    EXPECT_EQ(relay_->findPublishNamespaceSession(kTestNamespace), direct);
  });
  EXPECT_EQ(openHandle->namespaces.size(), 2u);
  EXPECT_EQ(peer100Handle->namespaces.size(), 1u);

  auto late = createMockSession();
  negotiateCluster(*late);
  auto lateHandle = std::make_shared<RecordingNamespacePublishHandle>();
  doSubscribeNamespace(late, kTestNamespace, true, lateHandle);
  ASSERT_EQ(lateHandle->namespaces.size(), 1u);
  const auto* hopPath =
      lateHandle->namespaces.front().params.getFirstParam(TrackRequestParamKey::HOP_PATH);
  ASSERT_NE(hopPath, nullptr);
  EXPECT_EQ(
      decodeRelayHopPath(hopPath->asString, kVersionDraft18).value(),
      (std::vector<uint64_t>{200, kRelayHop})
  );

  verifyOnRelayExec([&] { relay_->doPublishNamespaceDone(kTestNamespace, direct); });
  driveIfMultiThread();
  EXPECT_TRUE(openHandle->dones.empty());
  EXPECT_EQ(peer100Handle->dones.size(), 1u);

  verifyOnRelayExec([&] { relay_->doPublishNamespaceDone(kTestNamespace, viaPeer); });
  driveIfMultiThread();
  EXPECT_EQ(openHandle->dones.size(), 1u);
  EXPECT_EQ(peer100Handle->dones.size(), 1u);

  for (const auto& session : {viaPeer, direct, open, peer100, late}) {
    removeSession(session);
  }
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
