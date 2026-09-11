/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MoqxRelayTestFixture.h"
#include <atomic>
#include <thread>

namespace openmoq::moqx::test {

namespace {
PublishNamespace clusterAd(std::vector<uint64_t> path, uint64_t cost) {
  PublishNamespace pub;
  pub.trackNamespace = kTestNamespace;
  pub.params.insertParam(Parameter(
      folly::to_underlying(TrackRequestParamKey::HOP_PATH),
      encodeRelayHopPath(path, kVersionDraft18).value()
  ));
  pub.params.insertParam(Parameter(folly::to_underlying(TrackRequestParamKey::ROUTE_COST), cost));
  return pub;
}
class ClusterCostObserver : public Publisher::NamespacePublishHandle {
public:
  std::atomic<uint64_t> cost{UINT64_MAX};
  std::atomic<uint64_t> updates{0};
  void namespaceMsg(const Namespace& ns) override {
    auto value = ns.params.getFirstParam(TrackRequestParamKey::ROUTE_COST);
    cost = value ? value->asUint64 : 0;
    ++updates;
  }
  void namespaceMsg(const TrackNamespace&) override {}
  void namespaceDoneMsg(const TrackNamespace&) override {}
};
class ClusterPeerSession : public NiceMock<MockMoQSession> {
public:
  explicit ClusterPeerSession(std::shared_ptr<MoQExecutor> exec)
      : NiceMock<MockMoQSession>(std::move(exec)) {}

  folly::coro::Task<Publisher::SubscribeNamespaceResult>
  subscribeNamespace(SubscribeNamespace request, std::shared_ptr<Publisher::NamespacePublishHandle>)
      override {
    ++namespaceRequests;
    if (namespaceGate) {
      co_await *namespaceGate;
    }
    auto handle = std::make_shared<NiceMock<MockSubscribeNamespaceHandle>>(
        SubscribeNamespaceOk{.requestID = request.requestID}
    );
    namespaceHandle = handle;
    co_return handle;
  }

  folly::coro::Task<Publisher::SubscribeTracksResult>
  subscribeTracks(SubscribeTracks request, std::shared_ptr<Publisher::PublishBlockedHandle>)
      override {
    ++tracksRequests;
    if (tracksGate) {
      co_await *tracksGate;
    }
    EXPECT_TRUE(request.trackNamespacePrefix.empty());
    EXPECT_FALSE(request.forward);
    auto handle = std::make_shared<NiceMock<MockSubscribeTracksHandle>>(
        SubscribeTracksOk{.requestID = request.requestID}
    );
    tracksHandle = handle;
    co_return handle;
  }

  folly::coro::Baton* namespaceGate{nullptr};
  folly::coro::Baton* tracksGate{nullptr};
  std::atomic<size_t> namespaceRequests{0};
  std::atomic<size_t> tracksRequests{0};
  std::weak_ptr<Publisher::SubscribeNamespaceHandle> namespaceHandle;
  std::weak_ptr<Publisher::SubscribeTracksHandle> tracksHandle;
};
} // namespace

TEST_P(MoQRelayTest, ClusterPeerTrackDiscoveryUsesDraft18AndReleasesOnDisconnect) {
  for (auto version : {kVersionDraft16, kVersionDraft18}) {
    auto session = std::make_shared<ClusterPeerSession>(exec_);
    ON_CALL(*session, getNegotiatedVersion())
        .WillByDefault(Return(std::optional<uint64_t>(version)));
    auto* target =
        relay_->getRelayExec() ? relay_->getRelayExec() : static_cast<MoQExecutor*>(exec_.get());
    folly::coro::blockingWait(
        folly::coro::co_withExecutor(
            folly::getKeepAliveToken(target),
            relay_->onUpstreamConnect(session)
        ),
        exec_.get()
    );
    EXPECT_EQ(session->namespaceRequests.load(), 1);
    EXPECT_EQ(session->tracksRequests.load(), version == kVersionDraft18 ? 1 : 0);
    relay_->onSessionEnd(session);
    drainExecs();
    EXPECT_TRUE(session->tracksHandle.expired());
  }
}

TEST_P(MoQRelayTest, ClusterStopReleasesPeerHandlesWithoutDisconnectCallback) {
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "local-peer", 900);
  auto outgoing = std::make_shared<ClusterPeerSession>(exec_);
  auto incoming = std::make_shared<ClusterPeerSession>(exec_);
  for (auto session : {outgoing, incoming}) {
    ON_CALL(*session, getNegotiatedVersion())
        .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft18)));
  }
  auto* target =
      relay_->getRelayExec() ? relay_->getRelayExec() : static_cast<MoQExecutor*>(exec_.get());
  folly::coro::blockingWait(
      folly::coro::co_withExecutor(
          folly::getKeepAliveToken(target),
          relay_->onUpstreamConnect(outgoing)
      ),
      exec_.get()
  );
  auto subscription = withSessionContext(incoming, [&] {
    return folly::coro::blockingWait(
        publisherInterface()->subscribeNamespace(
            makePeerSubNs("remote-peer"),
            std::make_shared<ClusterCostObserver>()
        ),
        exec_.get()
    );
  });
  ASSERT_TRUE(subscription.hasValue());
  EXPECT_FALSE(outgoing->tracksHandle.expired());
  EXPECT_FALSE(incoming->tracksHandle.expired());
  relay_->stop();
  drainExecs();
  for (auto session : {outgoing, incoming}) {
    EXPECT_TRUE(session->namespaceHandle.expired());
    EXPECT_TRUE(session->tracksHandle.expired());
  }
  subscription.value()->unsubscribeNamespace();
  drainExecs();
  removeSession(incoming);
}

TEST_P(MoQRelayTest, ClusterReciprocalPeerDiscoversTracks) {
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "local-peer", 900);
  auto session = std::make_shared<ClusterPeerSession>(exec_);
  ON_CALL(*session, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft18)));
  auto handle = withSessionContext(session, [&] {
    return folly::coro::blockingWait(
        publisherInterface()->subscribeNamespace(
            makePeerSubNs("remote-peer"),
            std::make_shared<ClusterCostObserver>()
        ),
        exec_.get()
    );
  });
  ASSERT_TRUE(handle.hasValue());
  EXPECT_EQ(session->namespaceRequests.load(), 1);
  EXPECT_EQ(session->tracksRequests.load(), 1);
  handle.value()->unsubscribeNamespace();
  drainExecs();
  EXPECT_TRUE(session->tracksHandle.expired());
  removeSession(session);
}

TEST_P(MoQRelayTest, ClusterPeerDisconnectFencesLateHandshakeSuccess) {
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "local-peer", 900);
  for (int scenario : {0, 1, 2}) {
    const bool reciprocal = scenario != 0;
    for (bool parkTracks : {false, true}) {
      if (scenario == 2 && !parkTracks) {
        continue;
      }
      auto session = std::make_shared<ClusterPeerSession>(exec_);
      ON_CALL(*session, getNegotiatedVersion())
          .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft18)));
      folly::coro::Baton gate;
      (parkTracks ? session->tracksGate : session->namespaceGate) = &gate;
      std::atomic<bool> done{false};
      auto* target =
          relay_->getRelayExec() ? relay_->getRelayExec() : static_cast<MoQExecutor*>(exec_.get());
      auto pump = [&](auto ready) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!ready() && std::chrono::steady_clock::now() < deadline) {
          exec_->drive();
          std::this_thread::yield();
        }
        return ready();
      };
      std::shared_ptr<Publisher::SubscribeNamespaceHandle> previousNamespace;
      if (scenario == 2) {
        auto previous = withSessionContext(session, [&] {
          return folly::coro::blockingWait(
              publisherInterface()
                  ->subscribeNamespace(makePeerSubNs(), std::make_shared<ClusterCostObserver>()),
              exec_.get()
          );
        });
        ASSERT_TRUE(previous.hasValue());
        previousNamespace = std::move(previous.value());
      }
      withSessionContext(session, [&] {
        std::optional<folly::coro::Task<Publisher::SubscribeNamespaceResult>> incoming;
        if (reciprocal) {
          incoming.emplace(publisherInterface()->subscribeNamespace(
              makePeerSubNs("remote-peer"),
              std::make_shared<ClusterCostObserver>()
          ));
        }
        co_withExecutor(
            folly::getKeepAliveToken(target),
            folly::coro::co_invoke(
                [&, incoming = std::move(incoming)]() mutable -> folly::coro::Task<void> {
                  if (reciprocal) {
                    auto result = co_await std::move(*incoming);
                    EXPECT_TRUE(result.hasError());
                  } else {
                    co_await relay_->onUpstreamConnect(session);
                  }
                  done = true;
                }
            )
        ).start();
      });
      EXPECT_TRUE(pump([&] {
        return parkTracks ? session->tracksRequests.load() == 1
                          : session->namespaceRequests.load() == 1;
      }));
      if (scenario == 2) {
        // Cancelling the reciprocal namespace also owns the pending tracks request.
        previousNamespace->unsubscribeNamespace();
      } else {
        relay_->onSessionEnd(session);
      }
      drainExecs();
      gate.post();
      ASSERT_TRUE(pump([&] { return done.load(); }));
      drainExecs();
      EXPECT_EQ(session->tracksRequests.load(), parkTracks ? 1 : 0);
      EXPECT_TRUE(session->tracksHandle.expired());
      EXPECT_TRUE(session->namespaceHandle.expired());
      removeSession(session);
    }
  }
}

TEST_P(MoQRelayTest, ClusterSubscribeDoesNotEchoExactPublishedTrack) {
  auto publisher = createMockSession();
  doPublishNamespace(publisher, kTestNamespace);
  doPublish(publisher, kTestTrackName);
  auto result = subscribeToTrack(
      publisher,
      kTestTrackName,
      createMockConsumer(),
      RequestID(18),
      false,
      SubscribeErrorCode::DOES_NOT_EXIST
  );
  EXPECT_EQ(result, nullptr);
  removeSession(publisher);
}

TEST_P(MoQRelayTest, ClusterLocalHopCollisionCannotSubscribe) {
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", 900);
  auto source = createMockSession();
  auto reader = createMockSession();
  ON_CALL(*reader, getPeerHopID()).WillByDefault(Return(uint64_t{900}));
  doPublishNamespace(source, kTestNamespace);
  doPublish(source, kTestTrackName);
  EXPECT_CALL(*source, subscribe(_, _)).Times(0);
  EXPECT_EQ(
      subscribeToTrack(
          reader,
          kTestTrackName,
          createMockConsumer(),
          RequestID(19),
          false,
          SubscribeErrorCode::DOES_NOT_EXIST
      ),
      nullptr
  );
  removeSession(reader);
  removeSession(source);
}

TEST_P(MoQRelayTest, ClusterTrackStatusDoesNotEchoExactPublishedTrack) {
  auto publisher = createMockSession();
  doPublishNamespace(publisher, kTestNamespace);
  doPublish(publisher, kTestTrackName);
  EXPECT_CALL(*publisher, trackStatus(_)).Times(0);
  TrackStatus req;
  req.fullTrackName = kTestTrackName;
  req.requestID = RequestID(20);
  withSessionContext(publisher, [&] {
    auto result = folly::coro::blockingWait(publisherInterface()->trackStatus(req), exec_.get());
    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(result.error().errorCode, TrackStatusErrorCode::DOES_NOT_EXIST);
  });
  removeSession(publisher);
}

TEST_P(MoQRelayTest, ClusterRequestsCoalesceOnlyOnEligibleRoute) {
  auto sourceA = createMockSession();
  auto sourceB = createMockSession();
  auto readerA = createMockSession();
  auto readerA2 = createMockSession();
  auto readerB = createMockSession();
  ON_CALL(*readerB, getPeerHopID()).WillByDefault(Return(uint64_t{200}));
  auto advertise = [&](const auto& source, const std::vector<uint64_t>& path, uint64_t cost) {
    ON_CALL(*source, getNegotiatedVersion())
        .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft18)));
    ON_CALL(*source, negotiatedSetupExtension(SetupExtension::RelayHops))
        .WillByDefault(Return(true));
    PublishNamespace req;
    req.trackNamespace = kTestNamespace;
    req.params.insertParam(Parameter(
        folly::to_underlying(TrackRequestParamKey::HOP_PATH),
        encodeRelayHopPath(path, kVersionDraft18).value()
    ));
    req.params.insertParam(Parameter(folly::to_underlying(TrackRequestParamKey::ROUTE_COST), cost));
    withSessionContext(source, [&] {
      auto result = folly::coro::blockingWait(
          subscriberInterface()->publishNamespace(std::move(req), nullptr),
          exec_.get()
      );
      ASSERT_TRUE(result.hasValue());
      getOrCreateMockState(source)->publishNamespaceHandles.push_back(result.value());
    });
  };
  advertise(sourceA, {100, 200}, 1);
  advertise(sourceB, {100, 300}, 10);
  SubscribeOk ok;
  ok.requestID = RequestID(0);
  ok.trackAlias = TrackAlias(0);
  ok.largest = AbsoluteLocation{8, 2};
  auto upstreamA = std::make_shared<NiceMock<MockSubscriptionHandle>>(ok);
  auto upstreamB = std::make_shared<NiceMock<MockSubscriptionHandle>>(ok);
  EXPECT_CALL(*sourceA, subscribe(_, _)).Times(1).WillOnce([upstreamA](const auto&, auto) {
    return folly::coro::makeTask<Publisher::SubscribeResult>(upstreamA);
  });
  EXPECT_CALL(*sourceB, subscribe(_, _)).Times(1).WillOnce([upstreamB](const auto&, auto) {
    return folly::coro::makeTask<Publisher::SubscribeResult>(upstreamB);
  });
  auto a = subscribeToTrack(readerA, kTestTrackName, createMockConsumer(), RequestID(1));
  auto a2 = subscribeToTrack(readerA2, kTestTrackName, createMockConsumer(), RequestID(2));
  auto b = subscribeToTrack(readerB, kTestTrackName, createMockConsumer(), RequestID(3));
  ASSERT_NE(a, nullptr);
  ASSERT_NE(a2, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(a->subscribeOk().largest, ok.largest);
  EXPECT_EQ(a2->subscribeOk().largest, ok.largest);
  EXPECT_EQ(b->subscribeOk().largest, ok.largest);
  removeSession(readerA);
  removeSession(readerA2);
  removeSession(readerB);
  removeSession(sourceA);
  removeSession(sourceB);
}

TEST_P(MoQRelayTest, ClusterWarmCostFollowsLiveIngressAndPreservesCostUpdates) {
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", 900);
  auto source = createMockSession();
  auto observer = createMockSession();
  auto reader = createMockSession();
  for (auto session : {source, observer}) {
    ON_CALL(*session, getNegotiatedVersion())
        .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft18)));
    ON_CALL(*session, negotiatedSetupExtension(SetupExtension::RelayHops))
        .WillByDefault(Return(true));
  }
  auto costs = std::make_shared<ClusterCostObserver>();
  doSubscribeNamespace(observer, kTestNamespace, true, costs);
  auto ad = withSessionContext(source, [&] {
    return folly::coro::blockingWait(
        subscriberInterface()->publishNamespace(clusterAd({7, 10}, 5), nullptr),
        exec_.get()
    );
  });
  ASSERT_TRUE(ad.hasValue());
  ASSERT_TRUE(driveUntil([&] { return costs->cost == 6; }));
  SubscribeOk ok;
  ok.requestID = RequestID(0);
  ok.trackAlias = TrackAlias(0);
  auto upstream = std::make_shared<NiceMock<MockSubscriptionHandle>>(ok);
  std::atomic<size_t> cancellations{0};
  EXPECT_CALL(*upstream, unsubscribe()).WillOnce([&] { ++cancellations; });
  EXPECT_CALL(*source, subscribe(_, _)).WillOnce([upstream](const auto&, auto) {
    return folly::coro::makeTask<Publisher::SubscribeResult>(upstream);
  });
  auto downstream = createMockConsumer();
  EXPECT_CALL(*downstream, publishDone(_)).Times(0);
  auto subscription = subscribeToTrack(reader, kTestTrackName, downstream, RequestID(4));
  ASSERT_NE(subscription, nullptr);
  ASSERT_TRUE(driveUntil([&] { return costs->cost == 0; }));
  EXPECT_TRUE(ad.value()->publishNamespaceUpdate(clusterAd({7, 10}, 12)).hasValue());
  driveIfMultiThread();
  EXPECT_EQ(cancellations.load(), 0);
  EXPECT_EQ(costs->cost.load(), 0);
  subscription->unsubscribe();
  ASSERT_TRUE(driveUntil([&] { return cancellations == 1 && costs->cost == 13; }));
  ad.value()->publishNamespaceDone();
  for (auto session : {source, observer, reader}) {
    removeSession(session);
  }
}

TEST_P(MoQRelayTest, ClusterStopCancelsPendingWarmGrace) {
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", 900, std::chrono::milliseconds(50));
  auto source = createMockSession();
  auto observer = createMockSession();
  auto reader = createMockSession();
  for (auto session : {source, observer}) {
    ON_CALL(*session, getNegotiatedVersion())
        .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft18)));
    ON_CALL(*session, negotiatedSetupExtension(SetupExtension::RelayHops))
        .WillByDefault(Return(true));
  }
  auto costs = std::make_shared<ClusterCostObserver>();
  doSubscribeNamespace(observer, kTestNamespace, true, costs);
  auto ad = withSessionContext(source, [&] {
    return folly::coro::blockingWait(
        subscriberInterface()->publishNamespace(clusterAd({7, 10}, 5), nullptr),
        exec_.get()
    );
  });
  ASSERT_TRUE(ad.hasValue());
  SubscribeOk ok;
  ok.requestID = RequestID(0);
  ok.trackAlias = TrackAlias(0);
  auto upstream = std::make_shared<NiceMock<MockSubscriptionHandle>>(ok);
  EXPECT_CALL(*source, subscribe(_, _)).WillOnce([upstream](const auto&, auto) {
    return folly::coro::makeTask<Publisher::SubscribeResult>(upstream);
  });
  auto subscription = subscribeToTrack(reader, kTestTrackName, createMockConsumer(), RequestID(4));
  ASSERT_NE(subscription, nullptr);
  ASSERT_TRUE(driveUntil([&] { return costs->cost == 0; }));
  subscription->unsubscribe();
  drainExecs();
  relay_->stop();
  drainExecs();
  // The grace expiration would advertise the cold price if it survived stop.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  drainExecs();
  EXPECT_EQ(costs->cost.load(), 0);
  for (auto session : {source, observer, reader}) {
    removeSession(session);
  }
}

TEST_P(MoQRelayTest, ClusterOriginReplacementCancelsSubscriptionAndFetch) {
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", 900);
  auto source = createMockSession();
  auto reader = createMockSession();
  ON_CALL(*source, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft18)));
  ON_CALL(*source, negotiatedSetupExtension(SetupExtension::RelayHops)).WillByDefault(Return(true));
  auto ad = withSessionContext(source, [&] {
    return folly::coro::blockingWait(
        subscriberInterface()->publishNamespace(clusterAd({7, 10}, 2), nullptr),
        exec_.get()
    );
  });
  ASSERT_TRUE(ad.hasValue());
  SubscribeOk ok;
  ok.requestID = RequestID(0);
  ok.trackAlias = TrackAlias(0);
  auto upstream = std::make_shared<NiceMock<MockSubscriptionHandle>>(ok);
  auto upstreamFetch = std::make_shared<NiceMock<MockFetchHandle>>(
      FetchOk{RequestID(5), GroupOrder::OldestFirst, 0, AbsoluteLocation{1, 0}, {}}
  );
  std::atomic<size_t> subCancelled{0}, fetchCancelled{0}, downstreamEnded{0}, downstreamReset{0};
  EXPECT_CALL(*upstream, unsubscribe()).WillOnce([&] { ++subCancelled; });
  EXPECT_CALL(*upstreamFetch, fetchCancel()).WillOnce([&] { ++fetchCancelled; });
  EXPECT_CALL(*source, subscribe(_, _)).WillOnce([upstream](const auto&, auto) {
    return folly::coro::makeTask<Publisher::SubscribeResult>(upstream);
  });
  std::shared_ptr<FetchConsumer> fetchIngress;
  EXPECT_CALL(*source, fetch(_, _)).WillOnce([&](Fetch, std::shared_ptr<FetchConsumer> consumer) {
    fetchIngress = std::move(consumer);
    return folly::coro::makeTask<Publisher::FetchResult>(upstreamFetch);
  });
  auto downstream = createMockConsumer();
  EXPECT_CALL(*downstream, publishDone(_)).WillOnce([&](auto) {
    ++downstreamEnded;
    return folly::makeExpected<MoQPublishError>(folly::unit);
  });
  auto fetchConsumer = std::make_shared<NiceMock<MockFetchConsumer>>();
  EXPECT_CALL(*fetchConsumer, reset(ResetStreamErrorCode::CANCELLED)).WillOnce([&](auto) {
    ++downstreamReset;
  });
  EXPECT_CALL(*fetchConsumer, object(_, _, _, _, _, _, _)).Times(0);
  ASSERT_NE(subscribeToTrack(reader, kTestTrackName, downstream, RequestID(4)), nullptr);
  auto fetchResult = withSessionContext(reader, [&] {
    return folly::coro::blockingWait(
        publisherInterface()->fetch(
            Fetch(RequestID(5), kTestTrackName, AbsoluteLocation{0, 0}, AbsoluteLocation{1, 0}),
            fetchConsumer
        ),
        exec_.get()
    );
  });
  ASSERT_TRUE(fetchResult.hasValue());
  EXPECT_TRUE(ad.value()->publishNamespaceUpdate(clusterAd({8, 10}, 2)).hasValue());
  ASSERT_TRUE(driveUntil([&] {
    return subCancelled == 1 && fetchCancelled == 1 && downstreamEnded == 1 && downstreamReset == 1;
  }));
  // Late source data cannot cross the replaced content generation.
  fetchIngress->object(0, 0, 0, folly::IOBuf::copyBuffer("old"), noExtensions(), true, false);
  fetchIngress->endOfFetch();
  driveIfMultiThread();
  fetchResult.value()->fetchCancel();
  ad.value()->publishNamespaceDone();
  removeSession(reader);
  removeSession(source);
}

TEST_P(MoQRelayTest, ClusterParentReplacementPreservesAuthoritativeChild) {
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", 900);
  auto parent = createMockSession();
  auto child = createMockSession();
  auto reader = createMockSession();
  for (auto source : {parent, child}) {
    ON_CALL(*source, getNegotiatedVersion())
        .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft18)));
    ON_CALL(*source, negotiatedSetupExtension(SetupExtension::RelayHops))
        .WillByDefault(Return(true));
  }
  auto parentAd = withSessionContext(parent, [&] {
    return folly::coro::blockingWait(
        subscriberInterface()->publishNamespace(clusterAd({7, 10}, 1), nullptr),
        exec_.get()
    );
  });
  TrackNamespace childNamespace = kTestNamespace;
  childNamespace.append("child");
  FullTrackName childTrack{childNamespace, "video"};
  auto childAdRequest = clusterAd({8, 11}, 1);
  childAdRequest.trackNamespace = childNamespace;
  auto childAd = withSessionContext(child, [&] {
    return folly::coro::blockingWait(
        subscriberInterface()->publishNamespace(childAdRequest, nullptr),
        exec_.get()
    );
  });
  ASSERT_TRUE(parentAd.hasValue());
  ASSERT_TRUE(childAd.hasValue());
  auto ingress = doPublish(child, childTrack);
  auto consumer = createMockConsumer();
  EXPECT_CALL(*consumer, publishDone(_)).Times(0);
  ASSERT_NE(subscribeToTrack(reader, childTrack, consumer), nullptr);
  EXPECT_TRUE(parentAd.value()->publishNamespaceUpdate(clusterAd({9, 10}, 1)).hasValue());
  driveIfMultiThread();
  verifyOnRelayExec([&] { EXPECT_EQ(relay_->findPublishState(childTrack).session, child); });
  auto subgroup = createMockSubgroupConsumer();
  std::atomic<bool> delivered{false};
  EXPECT_CALL(*consumer, beginSubgroup(1, 0, _, _))
      .WillOnce([&](uint64_t, uint64_t, uint8_t, BeginSubgroupOptions) {
        delivered = true;
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(subgroup);
      });
  auto result = ingress->beginSubgroup(1, 0, 0);
  ASSERT_TRUE(result.hasValue());
  EXPECT_TRUE(result.value()->endOfSubgroup().hasValue());
  EXPECT_TRUE(driveUntil([&] { return delivered.load(); }));
  ASSERT_TRUE(Mock::VerifyAndClearExpectations(consumer.get()));
  parentAd.value()->publishNamespaceDone();
  childAd.value()->publishNamespaceDone();
  for (auto session : {reader, child, parent}) {
    removeSession(session);
  }
}

TEST_P(MoQRelayTest, ClusterOverlappingNamespaceOwnerHandsOff) {
  auto source = createMockSession();
  auto observer = createMockSession();
  for (auto session : {source, observer}) {
    ON_CALL(*session, getNegotiatedVersion())
        .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft18)));
    ON_CALL(*session, negotiatedSetupExtension(SetupExtension::RelayHops))
        .WillByDefault(Return(true));
  }
  auto first = std::make_shared<ClusterCostObserver>();
  auto second = std::make_shared<ClusterCostObserver>();
  auto broad = doSubscribeNamespace(observer, kAllowedPrefix, true, first);
  auto narrow = doSubscribeNamespace(observer, kTestNamespace, true, second);
  auto advertisement = withSessionContext(source, [&] {
    return folly::coro::blockingWait(
        subscriberInterface()->publishNamespace(clusterAd({7, 10}, 5), nullptr),
        exec_.get()
    );
  });
  ASSERT_TRUE(advertisement.hasValue());
  ASSERT_TRUE(driveUntil([&] { return first->updates == 1; }));
  EXPECT_EQ(second->updates.load(), 0);
  broad->unsubscribeNamespace();
  ASSERT_TRUE(driveUntil([&] { return second->updates == 1; }));
  EXPECT_EQ(second->cost.load(), 6);
  EXPECT_EQ(first->updates.load(), 1);
  advertisement.value()->publishNamespaceDone();
  narrow->unsubscribeNamespace();
  removeSession(observer);
  removeSession(source);
}

TEST_P(MoQRelayTest, ClusterAnonymousReplacementRepeatsIdenticalAdvertisement) {
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", 900);
  auto reader = createMockSession();
  auto sourceA = createMockSession();
  auto sourceB = createMockSession();
  auto observer = createMockSession();
  ON_CALL(*observer, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft18)));
  ON_CALL(*observer, negotiatedSetupExtension(SetupExtension::RelayHops))
      .WillByDefault(Return(true));
  auto updates = std::make_shared<ClusterCostObserver>();
  doSubscribeNamespace(observer, kTestNamespace, true, updates);
  doPublishNamespace(sourceA, kTestNamespace);
  ASSERT_TRUE(driveUntil([&] { return updates->updates == 1; }));
  SubscribeOk ok;
  ok.requestID = RequestID(0);
  ok.trackAlias = TrackAlias(0);
  auto upstream = std::make_shared<NiceMock<MockSubscriptionHandle>>(ok);
  EXPECT_CALL(*sourceA, subscribe(_, _)).Times(2).WillRepeatedly([upstream](const auto&, auto) {
    return folly::coro::makeTask<Publisher::SubscribeResult>(upstream);
  });
  auto subscription = subscribeToTrack(reader, kTestTrackName, createMockConsumer(), RequestID(4));
  ASSERT_NE(subscription, nullptr);
  driveIfMultiThread();
  EXPECT_EQ(updates->cost.load(), 1);
  EXPECT_EQ(updates->updates.load(), 1);
  subscription->unsubscribe();
  driveIfMultiThread();
  EXPECT_EQ(updates->updates.load(), 1);
  subscription = subscribeToTrack(reader, kTestTrackName, createMockConsumer(), RequestID(8));
  ASSERT_NE(subscription, nullptr);
  driveIfMultiThread();
  EXPECT_EQ(updates->updates.load(), 1);
  doPublishNamespace(sourceB, kTestNamespace);
  ASSERT_TRUE(driveUntil([&] { return updates->updates >= 2; }));
  EXPECT_EQ(updates->updates.load(), 2);
  EXPECT_EQ(updates->cost.load(), 1);
  for (auto session : {reader, observer, sourceA, sourceB}) {
    removeSession(session);
  }
}

TEST_P(MoQRelayTest, ClusterPathUpdateDetachesOnlyExcludedReader) {
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", 900);
  auto source = createMockSession();
  auto excluded = createMockSession();
  auto retained = createMockSession();
  ON_CALL(*source, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft18)));
  ON_CALL(*source, negotiatedSetupExtension(SetupExtension::RelayHops)).WillByDefault(Return(true));
  ON_CALL(*excluded, getPeerHopID()).WillByDefault(Return(uint64_t{20}));
  ON_CALL(*retained, getPeerHopID()).WillByDefault(Return(uint64_t{30}));
  auto advertisement = withSessionContext(source, [&] {
    return folly::coro::blockingWait(
        subscriberInterface()->publishNamespace(clusterAd({7, 10}, 3), nullptr),
        exec_.get()
    );
  });
  ASSERT_TRUE(advertisement.hasValue());
  SubscribeOk ok;
  ok.requestID = RequestID(0);
  ok.trackAlias = TrackAlias(0);
  auto upstream = std::make_shared<NiceMock<MockSubscriptionHandle>>(ok);
  EXPECT_CALL(*upstream, unsubscribe()).Times(0);
  std::shared_ptr<TrackConsumer> ingress;
  EXPECT_CALL(*source, subscribe(_, _))
      .Times(1)
      .WillOnce([&](const auto&, std::shared_ptr<TrackConsumer> consumer) {
        ingress = std::move(consumer);
        return folly::coro::makeTask<Publisher::SubscribeResult>(upstream);
      });
  auto excludedConsumer = createMockConsumer();
  auto retainedConsumer = createMockConsumer();
  std::atomic<bool> ended{false}, delivered{false};
  EXPECT_CALL(*excludedConsumer, publishDone(_)).WillOnce([&](auto) {
    ended = true;
    return folly::makeExpected<MoQPublishError>(folly::unit);
  });
  EXPECT_CALL(*retainedConsumer, publishDone(_)).Times(0);
  ASSERT_NE(subscribeToTrack(excluded, kTestTrackName, excludedConsumer, RequestID(1)), nullptr);
  ASSERT_NE(subscribeToTrack(retained, kTestTrackName, retainedConsumer, RequestID(2)), nullptr);
  EXPECT_TRUE(advertisement.value()->publishNamespaceUpdate(clusterAd({7, 20, 10}, 3)).hasValue());
  ASSERT_TRUE(driveUntil([&] { return ended.load(); }));
  EXPECT_CALL(*excludedConsumer, datagram(_, _, _)).Times(0);
  EXPECT_CALL(*retainedConsumer, datagram(_, _, _))
      .WillOnce([&](const ObjectHeader&, Payload payload, bool) {
        EXPECT_EQ(payload->computeChainDataLength(), 4);
        delivered = true;
        return folly::makeExpected<MoQPublishError>(folly::unit);
      });
  ObjectHeader header;
  header.group = 1;
  header.id = 0;
  ingress->datagram(header, folly::IOBuf::copyBuffer("live"));
  EXPECT_TRUE(driveUntil([&] { return delivered.load(); }));
  ASSERT_TRUE(Mock::VerifyAndClearExpectations(upstream.get()));
  ASSERT_TRUE(Mock::VerifyAndClearExpectations(retainedConsumer.get()));
  advertisement.value()->publishNamespaceDone();
  for (auto session : {excluded, retained, source}) {
    removeSession(session);
  }
}

TEST_P(MoQRelayTest, ClusterJoiningFetchWaitsForMatchingSubscribeReady) {
  auto source = createMockSession();
  auto reader = createMockSession();
  doPublishNamespace(source, kTestNamespace);
  auto pump = [&](auto ready) {
    for (size_t i = 0; i < 1000 && !ready(); ++i) {
      exec_->drive();
    }
    return ready();
  };
  folly::coro::Baton gate;
  std::atomic<bool> subscribeStarted{false}, subscribeDone{false}, fetchStarted{false},
      fetchDone{false};
  SubscribeOk ok;
  ok.requestID = RequestID(1);
  ok.trackAlias = TrackAlias(1);
  ok.largest = AbsoluteLocation{4, 1};
  EXPECT_CALL(*source, subscribe(_, _))
      .WillOnce(
          [&](const SubscribeRequest&,
              std::shared_ptr<TrackConsumer>) -> folly::coro::Task<Publisher::SubscribeResult> {
            subscribeStarted = true;
            co_await gate;
            co_return std::make_shared<NiceMock<MockSubscriptionHandle>>(ok);
          }
      );
  EXPECT_CALL(*source, fetch(_, _))
      .WillOnce([&](Fetch fetch, std::shared_ptr<FetchConsumer> consumer) {
        fetchStarted = true;
        auto standalone = std::get_if<StandaloneFetch>(&fetch.args);
        EXPECT_NE(standalone, nullptr);
        if (standalone) {
          EXPECT_EQ(standalone->start, (AbsoluteLocation{2, 0}));
          EXPECT_EQ(standalone->end, (AbsoluteLocation{4, 2}));
        }
        consumer->endOfFetch();
        return folly::coro::makeTask<Publisher::FetchResult>(
            std::make_shared<NiceMock<MockFetchHandle>>(
                FetchOk{RequestID(2), GroupOrder::OldestFirst, 0, AbsoluteLocation{4, 2}, {}}
            )
        );
      });
  std::optional<Publisher::SubscribeResult> subscribeResult;
  std::optional<Publisher::FetchResult> fetchResult;
  withSessionContext(reader, [&] {
    SubscribeRequest request;
    request.fullTrackName = kTestTrackName;
    request.requestID = RequestID(1);
    request.locType = LocationType::LargestObject;
    auto task = publisherInterface()->subscribe(request, createMockConsumer());
    co_withExecutor(
        static_cast<folly::DrivableExecutor*>(exec_.get()),
        folly::coro::co_invoke([&, task = std::move(task)]() mutable -> folly::coro::Task<void> {
          subscribeResult = co_await std::move(task);
          subscribeDone = true;
        })
    ).start();
  });
  auto releaseGate = folly::makeGuard([&] { gate.post(); });
  ASSERT_TRUE(pump([&] { return subscribeStarted.load(); }));
  withSessionContext(reader, [&] {
    Fetch request(RequestID(2), RequestID(1), 2, FetchType::RELATIVE_JOINING);
    request.fullTrackName = kTestTrackName;
    auto consumer = std::make_shared<NiceMock<MockFetchConsumer>>();
    ON_CALL(*consumer, endOfFetch())
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
    auto task = publisherInterface()->fetch(std::move(request), consumer);
    co_withExecutor(
        static_cast<folly::DrivableExecutor*>(exec_.get()),
        folly::coro::co_invoke([&, task = std::move(task)]() mutable -> folly::coro::Task<void> {
          fetchResult = co_await std::move(task);
          fetchDone = true;
        })
    ).start();
  });
  exec_->drive();
  EXPECT_FALSE(fetchStarted.load());
  releaseGate.dismiss();
  gate.post();
  ASSERT_TRUE(pump([&] { return subscribeDone && fetchDone; }));
  ASSERT_TRUE(subscribeResult->hasValue());
  ASSERT_TRUE(fetchResult->hasValue());
  subscribeResult->value()->unsubscribe();
  removeSession(reader);
  removeSession(source);
}

} // namespace openmoq::moqx::test
