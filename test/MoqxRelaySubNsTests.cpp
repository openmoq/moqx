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

PublishNamespace
clusterAdvert(std::vector<uint64_t> path, uint64_t cost, TrackNamespace ns = kTestNamespace) {
  PublishNamespace pubNs;
  pubNs.trackNamespace = std::move(ns);
  pubNs.params.insertParam(Parameter(
      folly::to_underlying(TrackRequestParamKey::HOP_PATH),
      encodeRelayHopPath(path, kVersionDraft18).value()
  ));
  pubNs.params.insertParam(Parameter(folly::to_underlying(TrackRequestParamKey::ROUTE_COST), cost));
  return pubNs;
}

std::vector<uint64_t> hopPathOf(const Namespace& ns) {
  const auto* param = ns.params.getFirstParam(TrackRequestParamKey::HOP_PATH);
  return param ? decodeRelayHopPath(param->asString, kVersionDraft18).value()
               : std::vector<uint64_t>{};
}

// One session may hold nested SUBSCRIBE_NAMESPACE prefixes. Each is told the namespace
// on its own, and unsubscribing one leaves the other's view as it was.
TEST_P(MoQRelayTest, NestedPrefixesFromOneSessionAreTrackedApart) {
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", 900);
  auto subscriber = createMockSession();
  negotiateCluster(*subscriber);
  auto outer = std::make_shared<RecordingNamespacePublishHandle>();
  doSubscribeNamespace(subscriber, TrackNamespace{{"test"}}, true, outer);
  auto inner = std::make_shared<RecordingNamespacePublishHandle>();
  auto innerSub = doSubscribeNamespace(subscriber, kTestNamespace, false, inner);

  auto publisher = createMockSession();
  negotiateCluster(*publisher);
  doPublishNamespaceMsg(publisher, clusterAdvert({100}, 5));
  driveIfMultiThread();
  EXPECT_EQ(outer->namespaces.size(), 1u);
  EXPECT_EQ(inner->namespaces.size(), 1u);

  doPublishNamespaceMsg(publisher, clusterAdvert({100}, 1));
  driveIfMultiThread();
  EXPECT_EQ(outer->namespaces.size(), 2u);
  EXPECT_EQ(inner->namespaces.size(), 2u);

  innerSub->unsubscribeNamespace();
  driveIfMultiThread();
  verifyOnRelayExec([&] { relay_->doPublishNamespaceDone(kTestNamespace, publisher); });
  driveIfMultiThread();
  EXPECT_EQ(outer->dones.size(), 1u);
  EXPECT_TRUE(inner->dones.empty());

  removeSession(publisher);
  removeSession(subscriber);
  driveIfMultiThread();
}

// Each subscriber is told the best route it may use. Moving to a different
// publisher withdraws the old advertisement first.
TEST_P(MoQRelayTest, EachSubscriberIsAdvertisedItsBestRoute) {
  constexpr uint64_t kRelayHop = 900;
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", kRelayHop);
  auto viaPeer = createMockSession();
  negotiateCluster(*viaPeer);
  auto direct = createMockSession();
  negotiateCluster(*direct);

  // open may use both routes; peer100 is excluded from the one through hop 100.
  auto open = createMockSession();
  negotiateCluster(*open);
  auto openHandle = std::make_shared<RecordingNamespacePublishHandle>();
  doSubscribeNamespace(open, kTestNamespace, true, openHandle);
  auto peer100 = createMockSession();
  negotiateCluster(*peer100, 100);
  auto peer100Handle = std::make_shared<RecordingNamespacePublishHandle>();
  doSubscribeNamespace(peer100, kTestNamespace, true, peer100Handle);

  doPublishNamespaceMsg(viaPeer, clusterAdvert({100}, 5));
  driveIfMultiThread();
  ASSERT_EQ(openHandle->namespaces.size(), 1u);
  EXPECT_EQ(hopPathOf(openHandle->namespaces[0]), (std::vector<uint64_t>{100, kRelayHop}));
  EXPECT_TRUE(peer100Handle->namespaces.empty());

  doPublishNamespaceMsg(direct, clusterAdvert({200}, 1));
  driveIfMultiThread();
  verifyOnRelayExec([&] {
    EXPECT_EQ(relay_->findPublishNamespaceSessions(kTestNamespace).size(), 2u);
  });
  EXPECT_EQ(openHandle->dones.size(), 1u);
  ASSERT_EQ(openHandle->namespaces.size(), 2u);
  EXPECT_EQ(hopPathOf(openHandle->namespaces[1]), (std::vector<uint64_t>{200, kRelayHop}));
  ASSERT_EQ(peer100Handle->namespaces.size(), 1u);
  EXPECT_EQ(hopPathOf(peer100Handle->namespaces[0]), (std::vector<uint64_t>{200, kRelayHop}));

  auto late = createMockSession();
  negotiateCluster(*late);
  auto lateHandle = std::make_shared<RecordingNamespacePublishHandle>();
  doSubscribeNamespace(late, kTestNamespace, true, lateHandle);
  ASSERT_EQ(lateHandle->namespaces.size(), 1u);
  EXPECT_EQ(hopPathOf(lateHandle->namespaces[0]), (std::vector<uint64_t>{200, kRelayHop}));

  verifyOnRelayExec([&] { relay_->doPublishNamespaceDone(kTestNamespace, direct); });
  driveIfMultiThread();
  EXPECT_EQ(openHandle->dones.size(), 2u);
  ASSERT_EQ(openHandle->namespaces.size(), 3u);
  EXPECT_EQ(hopPathOf(openHandle->namespaces[2]), (std::vector<uint64_t>{100, kRelayHop}));
  EXPECT_EQ(peer100Handle->dones.size(), 1u);

  verifyOnRelayExec([&] { relay_->doPublishNamespaceDone(kTestNamespace, viaPeer); });
  driveIfMultiThread();
  EXPECT_EQ(openHandle->dones.size(), 3u);
  EXPECT_EQ(peer100Handle->dones.size(), 1u);
  EXPECT_EQ(peer100Handle->namespaces.size(), 1u);

  for (const auto& session : {viaPeer, direct, open, peer100, late}) {
    removeSession(session);
  }
  driveIfMultiThread();
}

// A better path from the same origin is an update. A subscriber that did not negotiate
// MoQ Cluster sees no path, so it hears nothing new.
TEST_P(MoQRelayTest, SameOriginRouteChangeIsAnUpdate) {
  constexpr uint64_t kRelayHop = 900;
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", kRelayHop);
  auto first = createMockSession();
  negotiateCluster(*first);
  auto second = createMockSession();
  negotiateCluster(*second);

  auto cluster = createMockSession();
  negotiateCluster(*cluster);
  auto clusterHandle = std::make_shared<RecordingNamespacePublishHandle>();
  doSubscribeNamespace(cluster, kTestNamespace, true, clusterHandle);
  auto plain = createMockSession();
  auto plainHandle = std::make_shared<RecordingNamespacePublishHandle>();
  doSubscribeNamespace(plain, kTestNamespace, true, plainHandle);

  doPublishNamespaceMsg(first, clusterAdvert({100, 5}, 5));
  driveIfMultiThread();
  doPublishNamespaceMsg(second, clusterAdvert({100, 6}, 2));
  driveIfMultiThread();
  // The same session lowering its own cost is an update too.
  doPublishNamespaceMsg(second, clusterAdvert({100, 6}, 1));
  driveIfMultiThread();

  EXPECT_TRUE(clusterHandle->dones.empty());
  ASSERT_EQ(clusterHandle->namespaces.size(), 3u);
  EXPECT_EQ(hopPathOf(clusterHandle->namespaces[1]), (std::vector<uint64_t>{100, 6, kRelayHop}));
  const auto* cost =
      clusterHandle->namespaces[2].params.getFirstParam(TrackRequestParamKey::ROUTE_COST);
  ASSERT_NE(cost, nullptr);
  EXPECT_EQ(cost->asUint64, 2u); // 1 plus the default link cost
  EXPECT_TRUE(plainHandle->dones.empty());
  EXPECT_EQ(plainHandle->namespaces.size(), 1u);

  for (const auto& session : {first, second, cluster, plain}) {
    removeSession(session);
  }
  driveIfMultiThread();
}

// Unsubscribing forgets what the subscription was told, so subscribing again
// replays the namespace.
TEST_P(MoQRelayTest, ResubscribeReplaysTheNamespace) {
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", 900);
  auto publisher = createMockSession();
  negotiateCluster(*publisher);
  doPublishNamespaceMsg(publisher, clusterAdvert({100}, 1));
  driveIfMultiThread();

  auto subscriber = createMockSession();
  negotiateCluster(*subscriber);
  auto firstHandle = std::make_shared<RecordingNamespacePublishHandle>();
  auto subscription = doSubscribeNamespace(subscriber, kTestNamespace, false, firstHandle);
  EXPECT_EQ(firstHandle->namespaces.size(), 1u);
  subscription->unsubscribeNamespace();
  driveIfMultiThread();

  auto secondHandle = std::make_shared<RecordingNamespacePublishHandle>();
  doSubscribeNamespace(subscriber, kTestNamespace, true, secondHandle);
  EXPECT_EQ(secondHandle->namespaces.size(), 1u);

  removeSession(publisher);
  removeSession(subscriber);
  driveIfMultiThread();
}

// A handle from a session's earlier PUBLISH_NAMESPACE cannot update or end the newer
// one. A malformed HOP_PATH in REQUEST_UPDATE closes the session.
TEST_P(MoQRelayTest, RequestUpdateRejectsStaleHandlesAndMalformedPaths) {
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", 900);
  auto publisher = createMockSession();
  negotiateCluster(*publisher);
  auto publishWith = [&](RequestID requestID) {
    auto pubNs = clusterAdvert({100}, 1);
    pubNs.requestID = requestID;
    auto result = withSessionContext(publisher, [&] {
      return folly::coro::blockingWait(
          subscriberInterface()->publishNamespace(
              std::move(pubNs),
              std::make_shared<NiceMock<MockPublishNamespaceCallback>>()
          ),
          exec_.get()
      );
    });
    EXPECT_TRUE(result.hasValue());
    driveIfMultiThread();
    return result.hasValue() ? result.value() : nullptr;
  };
  auto update = [&](const std::shared_ptr<Subscriber::PublishNamespaceHandle>& handle,
                    std::string hopPath) {
    RequestUpdate req;
    req.requestID = RequestID(7);
    req.params.insertParam(
        Parameter(folly::to_underlying(TrackRequestParamKey::HOP_PATH), std::move(hopPath))
    );
    auto result = folly::coro::blockingWait(handle->requestUpdate(std::move(req)), exec_.get());
    driveIfMultiThread();
    return result;
  };

  auto stale = publishWith(RequestID(1));
  auto current = publishWith(RequestID(3));
  ASSERT_NE(stale, nullptr);
  ASSERT_NE(current, nullptr);

  EXPECT_TRUE(update(stale, encodeRelayHopPath({300}, kVersionDraft18).value()).hasError());
  stale->publishNamespaceDone();
  driveIfMultiThread();
  verifyOnRelayExec([&] {
    EXPECT_EQ(relay_->findPublishNamespaceSession(kTestNamespace), publisher);
  });

  // A truncated two-byte draft-18 varint.
  EXPECT_TRUE(update(current, std::string("\x80", 1)).hasError());
  EXPECT_TRUE(publisher->isClosed());

  removeSession(publisher);
  driveIfMultiThread();
}

// A repeated NAMESPACE whose path now runs through this relay ends the route
// it would update, as a looped REQUEST_UPDATE does.
TEST_P(MoQRelayTest, LoopedRepeatNamespaceEndsTheRoute) {
  constexpr uint64_t kRelayHop = 900;
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", kRelayHop);
  auto peer = createMockSession();
  negotiateCluster(*peer);
  auto subscriber = createMockSession();
  negotiateCluster(*subscriber);
  auto namespaceHandle = std::make_shared<RecordingNamespacePublishHandle>();
  doSubscribeNamespace(subscriber, kTestNamespace, true, namespaceHandle);

  doPublishNamespaceMsg(peer, clusterAdvert({100}, 1));
  driveIfMultiThread();
  ASSERT_EQ(namespaceHandle->namespaces.size(), 1u);

  PublishNamespace looped = clusterAdvert({100, kRelayHop}, 1);
  withSessionContext(peer, [&] {
    folly::coro::blockingWait(
        subscriberInterface()->publishNamespace(std::move(looped), nullptr),
        exec_.get()
    );
  });
  driveIfMultiThread();
  EXPECT_EQ(namespaceHandle->dones.size(), 1u);
  verifyOnRelayExec([&] { EXPECT_EQ(relay_->findPublishNamespaceSession(kTestNamespace), nullptr); }
  );

  removeSession(peer);
  removeSession(subscriber);
  driveIfMultiThread();
}

// REQUEST_UPDATE on a PUBLISH_NAMESPACE. A new cost from the same origin is an
// update in place: subscriptions stay up. A new origin withdraws and
// re-advertises, and drops the subscriptions it served. A looped path ends it.
TEST_P(MoQRelayTest, RequestUpdateRevisesTheRoute) {
  constexpr uint64_t kRelayHop = 900;
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", kRelayHop);
  auto publisher = createMockSession();
  negotiateCluster(*publisher);
  auto subscriber = createMockSession();
  negotiateCluster(*subscriber);
  auto namespaceHandle = std::make_shared<RecordingNamespacePublishHandle>();
  doSubscribeNamespace(subscriber, kTestNamespace, true, namespaceHandle);

  auto handle = doPublishNamespaceMsg(publisher, clusterAdvert({100}, 5));
  ASSERT_NE(handle, nullptr);
  doPublish(publisher, kTestTrackName);
  driveIfMultiThread();
  ASSERT_EQ(namespaceHandle->namespaces.size(), 1u);

  auto update = [&](std::optional<std::vector<uint64_t>> path, std::optional<uint64_t> cost) {
    RequestUpdate req;
    req.requestID = RequestID(7);
    if (path) {
      req.params.insertParam(Parameter(
          folly::to_underlying(TrackRequestParamKey::HOP_PATH),
          encodeRelayHopPath(*path, kVersionDraft18).value()
      ));
    }
    if (cost) {
      req.params.insertParam(
          Parameter(folly::to_underlying(TrackRequestParamKey::ROUTE_COST), *cost)
      );
    }
    auto result = folly::coro::blockingWait(handle->requestUpdate(std::move(req)), exec_.get());
    driveIfMultiThread();
    return result;
  };
  auto tracks = [&] {
    size_t matched = 0;
    verifyOnRelayExec([&] { matched = relay_->matchTracks(kTestNamespace, nullptr, 10).matched; });
    return matched;
  };
  ASSERT_EQ(tracks(), 1u);

  ASSERT_TRUE(update(std::nullopt, 1).hasValue());
  EXPECT_TRUE(namespaceHandle->dones.empty());
  ASSERT_EQ(namespaceHandle->namespaces.size(), 2u);
  EXPECT_EQ(hopPathOf(namespaceHandle->namespaces[1]), (std::vector<uint64_t>{100, kRelayHop}));
  const auto* cost =
      namespaceHandle->namespaces[1].params.getFirstParam(TrackRequestParamKey::ROUTE_COST);
  ASSERT_NE(cost, nullptr);
  EXPECT_EQ(cost->asUint64, 2u); // 1 plus the default link cost
  EXPECT_EQ(tracks(), 1u);

  ASSERT_TRUE(update(std::vector<uint64_t>{300}, std::nullopt).hasValue());
  EXPECT_EQ(namespaceHandle->dones.size(), 1u);
  ASSERT_EQ(namespaceHandle->namespaces.size(), 3u);
  EXPECT_EQ(hopPathOf(namespaceHandle->namespaces[2]), (std::vector<uint64_t>{300, kRelayHop}));
  EXPECT_EQ(tracks(), 0u);

  auto looped = update(std::vector<uint64_t>{300, kRelayHop}, std::nullopt);
  ASSERT_TRUE(looped.hasError());
  EXPECT_EQ(looped.error().errorCode, RequestErrorCode::UNINTERESTED);
  EXPECT_EQ(namespaceHandle->dones.size(), 2u);
  verifyOnRelayExec([&] { EXPECT_EQ(relay_->findPublishNamespaceSession(kTestNamespace), nullptr); }
  );

  removeSession(publisher);
  removeSession(subscriber);
  driveIfMultiThread();
}

// A plain session's REQUEST_UPDATE cannot give its route a HOP_PATH or
// ROUTE_COST that outranks a MoQ Cluster route.
TEST_P(MoQRelayTest, PlainRequestUpdateKeepsTheAnonymousPath) {
  constexpr uint64_t kRelayHop = 900;
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", kRelayHop);
  auto cluster = createMockSession();
  negotiateCluster(*cluster);
  auto plain = createMockSession();
  auto subscriber = createMockSession();
  negotiateCluster(*subscriber);
  auto namespaceHandle = std::make_shared<RecordingNamespacePublishHandle>();
  doSubscribeNamespace(subscriber, kTestNamespace, true, namespaceHandle);

  doPublishNamespaceMsg(cluster, clusterAdvert({100}, 5));
  auto plainHandle = doPublishNamespace(plain, kTestNamespace);
  ASSERT_NE(plainHandle, nullptr);
  driveIfMultiThread();
  ASSERT_EQ(namespaceHandle->namespaces.size(), 1u);

  RequestUpdate req;
  req.requestID = RequestID(7);
  req.params.insertParam(Parameter(
      folly::to_underlying(TrackRequestParamKey::HOP_PATH),
      encodeRelayHopPath({7}, kVersionDraft18).value()
  ));
  req.params.insertParam(Parameter(folly::to_underlying(TrackRequestParamKey::ROUTE_COST), 0));
  auto result = folly::coro::blockingWait(plainHandle->requestUpdate(std::move(req)), exec_.get());
  driveIfMultiThread();

  EXPECT_TRUE(result.hasValue());
  EXPECT_TRUE(namespaceHandle->dones.empty());
  ASSERT_EQ(namespaceHandle->namespaces.size(), 1u);
  EXPECT_EQ(hopPathOf(namespaceHandle->namespaces[0]), (std::vector<uint64_t>{100, kRelayHop}));
  verifyOnRelayExec([&] { EXPECT_EQ(relay_->findPublishNamespaceSession(kTestNamespace), cluster); }
  );

  for (const auto& session : {cluster, plain, subscriber}) {
    removeSession(session);
  }
  driveIfMultiThread();
}

// NAMESPACE_TOO_LARGE counts only the namespaces that would be advertised.
TEST_P(MoQRelayTest, NamespaceLimitCountsOnlyAdvertisableNamespaces) {
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", 900);
  auto publisher = createMockSession();
  negotiateCluster(*publisher);
  for (size_t i = 0; i <= MoqxRelay::kMaxNamespaceMatches; ++i) {
    auto ns = kTestNamespace;
    ns.append("n" + std::to_string(i));
    doPublishNamespaceMsg(publisher, clusterAdvert({100}, 1, std::move(ns)), false);
  }
  driveIfMultiThread();

  auto subscribe = [&](const std::shared_ptr<MockMoQSession>& subscriber) {
    SubscribeNamespace subNs;
    subNs.trackNamespacePrefix = kTestNamespace;
    return withSessionContext(subscriber, [&] {
      return folly::coro::blockingWait(
          publisherInterface()->subscribeNamespace(
              std::move(subNs),
              std::make_shared<RecordingNamespacePublishHandle>()
          ),
          exec_.get()
      );
    });
  };
  auto excluded = createMockSession();
  negotiateCluster(*excluded, 100);
  EXPECT_TRUE(subscribe(excluded).hasValue());
  auto open = createMockSession();
  negotiateCluster(*open);
  auto tooLarge = subscribe(open);
  ASSERT_TRUE(tooLarge.hasError());
  EXPECT_EQ(tooLarge.error().errorCode, SubscribeNamespaceErrorCode::NAMESPACE_TOO_LARGE);

  removeSession(publisher);
  removeSession(excluded);
  removeSession(open);
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
                  /*filter=*/LocationFilter::nextObject()
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

// A forward toggle leaves the upstream subscription's range and priority as they were.
TEST_P(MoQRelayTest, ForwardRequestUpdateSetsOnlyForward) {
  auto pubSession = createMockSession();
  doPublishNamespace(pubSession, kTestNamespace);
  auto mockHandle = makePublishHandle();
  doPublishWithHandle(pubSession, kTestTrackName, mockHandle);

  EXPECT_CALL(*mockHandle, requestUpdateCalled(_)).WillOnce([](const RequestUpdate& u) {
    EXPECT_EQ(u.forward, std::optional<bool>(true));
    EXPECT_FALSE(u.filter.has_value());
    EXPECT_FALSE(u.priority.has_value());
  });

  auto subSession = createMockSession();
  setupPublishSucceeds(subSession);
  doSubscribeNamespaceWithForward(subSession, kTestNamespace, /*forward=*/true);
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
