/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MoqxRelayTestFixture.h"
#include "UpstreamProvider.h"

#include <folly/CancellationToken.h>
#include <folly/coro/Baton.h>
#include <folly/json.h>

#include "admin/JsonStateVisitors.h"

namespace openmoq::moqx::test {

namespace {

// Mirrors what the /state handler does for one service, minus the HTTP layer:
// the walk runs on the executor that owns relay state and writes JSON as it
// goes. Threshold of 1 so every item is flushed separately -- if a chunk
// boundary could corrupt the output, these tests would see it too.
class StateWalk {
public:
  explicit StateWalk(size_t threshold = 1)
      : writer_(
            [this](std::unique_ptr<folly::IOBuf> chunk) {
              body_ += chunk->moveToFbString().toStdString();
              return true;
            },
            threshold
        ),
        visitor_(writer_) {
    // dumpState emits a relay fragment, not a document; brace it so the result
    // can be parsed.
    writer_.json().beginObject();
  }

  admin::JsonRelayStateVisitor& visitor() { return visitor_; }

  folly::dynamic finish() {
    writer_.json().endObject();
    writer_.flush();
    return folly::parseJson(body_);
  }

private:
  std::string body_;
  admin::ChunkedJsonWriter writer_;
  admin::JsonRelayStateVisitor visitor_;
};

const folly::dynamic* findSubscription(const folly::dynamic& state, const std::string& trackName) {
  for (const auto& sub : state["subscriptions"]) {
    if (sub["track_name"].asString() == trackName) {
      return &sub;
    }
  }
  return nullptr;
}

} // namespace

class MoQRelayStateTest : public MoQRelayTest {
protected:
  // dumpState is synchronous and must run where relay state lives, which is
  // exactly what verifyOnRelayExec provides in every mode.
  folly::dynamic dumpState() {
    StateWalk walk;
    verifyOnRelayExec([&] { relay_->dumpState(walk.visitor()); });
    return walk.finish();
  }
};

TEST_P(MoQRelayStateTest, PeerRelayIsReportedAsAnAcceptedLink) {
  // A peer session that accepts the relay's reciprocal SUBSCRIBE_NAMESPACE.
  class PeerSession : public NiceMock<MockMoQSession> {
  public:
    using NiceMock<MockMoQSession>::NiceMock;
    folly::coro::Task<Publisher::SubscribeNamespaceResult> subscribeNamespace(
        SubscribeNamespace subNs,
        std::shared_ptr<Publisher::NamespacePublishHandle> /*handle*/
    ) override {
      co_return std::make_shared<NiceMock<MockSubscribeNamespaceHandle>>(
          SubscribeNamespaceOk{subNs.requestID}
      );
    }
  };
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "sg-sin-2-1");
  relay_->setAllowedNamespacePrefix(kAllowedPrefix);
  auto peer = std::make_shared<PeerSession>(exec_);
  ON_CALL(*peer, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft16)));
  getOrCreateMockState(peer);
  auto subscribe = [&](SubscribeNamespace subNs) {
    return withSessionContext(peer, [&] {
      auto result = folly::coro::blockingWait(
          publisherInterface()->subscribeNamespace(
              std::move(subNs),
              std::make_shared<NiceMock<MockNamespacePublishHandle>>()
          ),
          exec_.get()
      );
      EXPECT_TRUE(result.hasValue());
      return result.hasValue() ? *result : nullptr;
    });
  };
  auto peering = subscribe(makePeerSubNs("jp-osa-1"));
  // A second peering request on the session is refused while the first is open.
  auto duplicate = withSessionContext(peer, [&] {
    return folly::coro::blockingWait(
        publisherInterface()->subscribeNamespace(
            makePeerSubNs("jp-osa-1"),
            std::make_shared<NiceMock<MockNamespacePublishHandle>>()
        ),
        exec_.get()
    );
  });
  ASSERT_TRUE(duplicate.hasError());
  EXPECT_EQ(duplicate.error().errorCode, SubscribeNamespaceErrorCode::DUPLICATE_SUBSCRIPTION);
  SubscribeNamespace other = makePeerSubNs();
  other.trackNamespacePrefix = kAllowedPrefix;
  auto otherHandle = subscribe(std::move(other));
  driveIfMultiThread();

  struct Links : PeerVisitor {
    std::vector<PeerLink> links;
    void onPeer(const PeerLink& link) override { links.push_back(link); }
  };
  auto visitPeers = [&] {
    Links links;
    folly::coro::blockingWait(peering_->visitPeers(links), exec_.get());
    return links.links;
  };
  auto links = visitPeers();
  ASSERT_EQ(links.size(), 1u);
  EXPECT_EQ(links[0].direction, PeerLink::Direction::Accepted);
  EXPECT_EQ(links[0].relayID, "jp-osa-1");

  auto peerIdOf = [&] {
    std::optional<std::string> id;
    verifyOnRelayExec([&] {
      id = peering_->reciprocalMoqtPeerHandler()->peerIdOf(peer->sessionId());
    });
    return id;
  };
  auto holdsHandles = [&] {
    bool holds = false;
    verifyOnRelayExec([&] { holds = peering_->holdsSessionHandles(); });
    return holds;
  };
  // Ending another SUBSCRIBE_NAMESPACE from the peer leaves peering in place.
  withSessionContext(peer, [&] { otherHandle->unsubscribeNamespace(); });
  driveIfMultiThread();
  EXPECT_TRUE(holdsHandles());

  // Ending the peering request releases the reciprocal, and the session stays the peer's.
  withSessionContext(peer, [&] { peering->unsubscribeNamespace(); });
  driveIfMultiThread();
  EXPECT_FALSE(holdsHandles());
  EXPECT_EQ(peerIdOf(), "jp-osa-1");

  verifyOnRelayExec([&] { relay_->onSessionEnd(peer->sessionId()); });
  EXPECT_FALSE(peerIdOf());
  EXPECT_TRUE(visitPeers().empty());

  removeSession(peer);
  driveIfMultiThread();
}

// A peer's SUBSCRIBE_NAMESPACE that is cancelled during the reciprocal does not keep it.
TEST_P(MoQRelayStateTest, CancelledPeerRequestReleasesItsReciprocal) {
  // Accepts the reciprocal SUBSCRIBE_NAMESPACE, and holds its SUBSCRIBE_TRACKS reply until the
  // test posts the gate.
  class GatedPeer : public NiceMock<MockMoQSession> {
  public:
    using NiceMock<MockMoQSession>::NiceMock;
    folly::coro::Task<Publisher::SubscribeNamespaceResult> subscribeNamespace(
        SubscribeNamespace subNs,
        std::shared_ptr<Publisher::NamespacePublishHandle> /*handle*/
    ) override {
      co_return std::make_shared<NiceMock<MockSubscribeNamespaceHandle>>(
          SubscribeNamespaceOk{subNs.requestID}
      );
    }
    folly::coro::Task<Publisher::SubscribeTracksResult>
    subscribeTracks(SubscribeTracks subTracks, std::shared_ptr<Publisher::PublishBlockedHandle>)
        override {
      tracksRequested = true;
      co_await gate;
      SubscribeTracksOk ok;
      ok.requestID = subTracks.requestID;
      co_return std::make_shared<NiceMock<MockSubscribeTracksHandle>>(std::move(ok));
    }
    std::atomic<bool> tracksRequested{false};
    folly::coro::Baton gate;
  };
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "sg-sin-2-1");
  relay_->setAllowedNamespacePrefix(kAllowedPrefix);
  auto peer = std::make_shared<GatedPeer>(exec_);
  ON_CALL(*peer, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft18)));
  getOrCreateMockState(peer);

  folly::CancellationSource cancel;
  auto request = withSessionContext(peer, [&] {
    return folly::coro::co_withExecutor(
               sessionExec(),
               folly::coro::co_withCancellation(
                   cancel.getToken(),
                   publisherInterface()->subscribeNamespace(
                       makePeerSubNs("jp-osa-1"),
                       std::make_shared<NiceMock<MockNamespacePublishHandle>>()
                   )
               )
    )
        .start();
  });
  ASSERT_TRUE(driveUntil([&] { return peer->tracksRequested.load(); }));
  cancel.requestCancellation();
  peer->gate.post();
  ASSERT_TRUE(driveUntil([&] { return request.isReady(); }));
  driveIfMultiThread();

  bool holds = true;
  verifyOnRelayExec([&] { holds = peering_->holdsSessionHandles(); });
  EXPECT_FALSE(holds);

  // The cancelled request is no longer open, so the session can peer again.
  auto again = withSessionContext(peer, [&] {
    return folly::coro::blockingWait(
        publisherInterface()->subscribeNamespace(
            makePeerSubNs("jp-osa-1"),
            std::make_shared<NiceMock<MockNamespacePublishHandle>>()
        ),
        exec_.get()
    );
  });
  EXPECT_TRUE(again.hasValue());
  driveIfMultiThread();
  verifyOnRelayExec([&] { holds = peering_->holdsSessionHandles(); });
  EXPECT_TRUE(holds);
  removeSession(peer);
  driveIfMultiThread();
}

TEST_P(MoQRelayStateTest, EmptyRelayHasRootTreeAndNoSubscriptions) {
  auto state = dumpState();

  EXPECT_EQ(state["subscriptions"].size(), 0);
  // The walk always emits a root node, even with nothing published.
  ASSERT_TRUE(state.count("namespace_tree"));
  EXPECT_EQ(state["namespace_tree"]["children"].size(), 0);
  // maxCachedTracks = 0 in the fixture, so the cache section is absent.
  EXPECT_EQ(state.count("cache"), 0);
}

TEST_P(MoQRelayStateTest, PublishedTracksAppearInEveryMode) {
  auto publisherSession = createMockSession();
  const FullTrackName second{kTestNamespace, "track2"};
  doPublish(publisherSession, kTestTrackName);
  doPublish(publisherSession, second);
  driveIfMultiThread();

  auto state = dumpState();

  EXPECT_EQ(state["subscriptions"].size(), 2);
  const auto* first = findSubscription(state, "track1");
  ASSERT_NE(first, nullptr) << folly::toJson(state);
  EXPECT_TRUE((*first)["is_publish"].asBool());
  EXPECT_NE(findSubscription(state, "track2"), nullptr) << folly::toJson(state);

  removeSession(publisherSession);
  exec_->drive();
}

// The bug this whole change exists for: in LocalForwarder mode the registry's
// forwarder is owned by another executor, so reading these counters off it
// reported zeros. They are now counted by RelayIngestFilter on the relay
// executor, which sees every object in every mode.
TEST_P(MoQRelayStateTest, IngestCountersAreReportedInEveryMode) {
  auto publisherSession = createMockSession();
  auto subscriber = createMockSession();
  auto mockConsumer = createMockConsumer();
  auto mockSg = createMockSubgroupConsumer();

  EXPECT_CALL(*mockConsumer, beginSubgroup(0, 0, _, _))
      .WillRepeatedly([&](uint64_t, uint64_t, uint8_t, moxygen::BeginSubgroupOptions) {
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(mockSg);
      });
  EXPECT_CALL(*mockSg, object(_, _, _, _))
      .WillRepeatedly(Return(folly::makeExpected<MoQPublishError>(folly::unit)));

  auto publishConsumer = doPublish(publisherSession, kTestTrackName);
  subscribeToTrack(subscriber, kTestTrackName, mockConsumer, RequestID(1));

  auto sg = publishConsumer->beginSubgroup(0, 0, 0);
  ASSERT_TRUE(sg.hasValue()) << sg.error().describe();
  EXPECT_TRUE(sg.value()->object(0, folly::IOBuf::copyBuffer("abc")).hasValue());
  EXPECT_TRUE(sg.value()->object(1, folly::IOBuf::copyBuffer("de")).hasValue());
  EXPECT_TRUE(sg.value()->endOfSubgroup().hasValue());
  driveIfMultiThread();

  auto state = dumpState();

  const auto* sub = findSubscription(state, "track1");
  ASSERT_NE(sub, nullptr) << folly::toJson(state);
  EXPECT_EQ((*sub)["total_groups_received"].asInt(), 1);
  EXPECT_EQ((*sub)["total_objects_received"].asInt(), 2);
  ASSERT_TRUE(sub->count("largest")) << folly::toJson(*sub);
  EXPECT_EQ((*sub)["largest"]["group"].asInt(), 0);
  EXPECT_EQ((*sub)["largest"]["object"].asInt(), 1);

  removeSession(publisherSession);
  removeSession(subscriber);
  exec_->drive();
}

// Regression: the upstream can write objects in the same batch as its SUBSCRIBE_OK, while
// the relay is still setting up the first subscriber. In LocalForwarder mode a relay chain
// filter that reaches the forwarder before its downstream is wired opens a dead subgroup,
// and the forwarder drops the chain for the rest of the track.
TEST_P(MoQRelayStateTest, IngestCountersCoverObjectsSentBeforeSubscribeOk) {
  auto publisherSession = createMockSession();
  auto subSession = createMockSession();

  // Announce without publishing, so the first subscriber makes the relay pull the track.
  doPublishNamespace(publisherSession, kTestNamespace);

  auto mockConsumer = createMockConsumer();
  auto mockSg = createMockSubgroupConsumer();
  EXPECT_CALL(*mockConsumer, beginSubgroup(0, 0, _, _))
      .WillRepeatedly([&](uint64_t, uint64_t, uint8_t, moxygen::BeginSubgroupOptions) {
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(mockSg);
      });
  EXPECT_CALL(*mockSg, object(_, _, _, _))
      .WillRepeatedly(Return(folly::makeExpected<MoQPublishError>(folly::unit)));

  SubscribeOk upstreamOk;
  upstreamOk.requestID = RequestID(1);
  upstreamOk.trackAlias = TrackAlias(1);
  upstreamOk.expires = std::chrono::milliseconds(0);
  upstreamOk.groupOrder = GroupOrder::OldestFirst;

  std::shared_ptr<SubgroupConsumer> upstreamSg;
  EXPECT_CALL(*publisherSession, subscribe(_, _))
      .WillOnce([&](const SubscribeRequest&, std::shared_ptr<TrackConsumer> consumer) {
        // Write before answering: this object reaches the forwarder while the relay is
        // still inside the setup that installs the chain.
        auto sg = consumer->beginSubgroup(0, 0, 0);
        EXPECT_TRUE(sg.hasValue());
        upstreamSg = sg.value();
        EXPECT_TRUE(upstreamSg->object(0, folly::IOBuf::copyBuffer("abc")).hasValue());
        auto handle = std::make_shared<NiceMock<MockSubscriptionHandle>>(upstreamOk);
        return folly::coro::makeTask<Publisher::SubscribeResult>(
            folly::Expected<std::shared_ptr<SubscriptionHandle>, SubscribeError>(handle)
        );
      });

  subscribeToTrack(subSession, kTestTrackName, mockConsumer, RequestID(0));
  ASSERT_NE(upstreamSg, nullptr) << "relay should have issued an upstream subscribe";

  EXPECT_TRUE(upstreamSg->object(1, folly::IOBuf::copyBuffer("de")).hasValue());
  EXPECT_TRUE(upstreamSg->endOfSubgroup().hasValue());
  drainExecs();

  auto state = dumpState();

  const auto* sub = findSubscription(state, "track1");
  ASSERT_NE(sub, nullptr) << folly::toJson(state);
  EXPECT_EQ((*sub)["total_objects_received"].asInt(), 2) << folly::toJson(*sub);
  EXPECT_EQ((*sub)["total_groups_received"].asInt(), 1);

  removeSession(publisherSession);
  removeSession(subSession);
  exec_->drive();
}

// Subgroups of different groups interleave on the wire, so a one-deep "did the
// group change" check counts every switch as a new group. The MRU window
// shared with TrackStatsFilter is what keeps this at 2.
TEST_P(MoQRelayStateTest, InterleavedGroupsAreCountedOnce) {
  auto publisherSession = createMockSession();
  auto subscriber = createMockSession();
  auto mockConsumer = createMockConsumer();
  auto mockSg = createMockSubgroupConsumer();

  EXPECT_CALL(*mockConsumer, beginSubgroup(_, _, _, _))
      .WillRepeatedly([&](uint64_t, uint64_t, uint8_t, moxygen::BeginSubgroupOptions) {
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(mockSg);
      });
  EXPECT_CALL(*mockSg, object(_, _, _, _))
      .WillRepeatedly(Return(folly::makeExpected<MoQPublishError>(folly::unit)));

  auto publishConsumer = doPublish(publisherSession, kTestTrackName);
  subscribeToTrack(subscriber, kTestTrackName, mockConsumer, RequestID(1));

  auto first = publishConsumer->beginSubgroup(0, 0, 0);
  ASSERT_TRUE(first.hasValue()) << first.error().describe();
  auto second = publishConsumer->beginSubgroup(1, 0, 0);
  ASSERT_TRUE(second.hasValue()) << second.error().describe();
  EXPECT_TRUE(first.value()->object(0, folly::IOBuf::copyBuffer("a")).hasValue());
  EXPECT_TRUE(second.value()->object(0, folly::IOBuf::copyBuffer("b")).hasValue());
  EXPECT_TRUE(first.value()->object(1, folly::IOBuf::copyBuffer("c")).hasValue());
  EXPECT_TRUE(second.value()->object(1, folly::IOBuf::copyBuffer("d")).hasValue());
  EXPECT_TRUE(first.value()->endOfSubgroup().hasValue());
  EXPECT_TRUE(second.value()->endOfSubgroup().hasValue());
  driveIfMultiThread();

  auto state = dumpState();

  const auto* sub = findSubscription(state, "track1");
  ASSERT_NE(sub, nullptr) << folly::toJson(state);
  EXPECT_EQ((*sub)["total_groups_received"].asInt(), 2) << "two groups, four switches";
  EXPECT_EQ((*sub)["total_objects_received"].asInt(), 4);
  EXPECT_EQ((*sub)["largest"]["group"].asInt(), 1);
  EXPECT_EQ((*sub)["largest"]["object"].asInt(), 1);

  removeSession(publisherSession);
  removeSession(subscriber);
  exec_->drive();
}

TEST_P(MoQRelayStateTest, AnnouncedNamespaceAppearsInTree) {
  auto publisherSession = createMockSession();
  doPublishNamespace(publisherSession, kTestNamespace);
  driveIfMultiThread();

  auto state = dumpState();

  // kTestNamespace is {"test","namespace"} and kAllowedPrefix is {"test"}, so
  // the tree nests one level per tuple below the root.
  const auto& root = state["namespace_tree"];
  ASSERT_EQ(root["children"].size(), 1) << folly::toJson(state);
  const auto& test = root["children"]["test"];
  ASSERT_EQ(test["children"].size(), 1) << folly::toJson(test);
  const auto& inner = test["children"]["namespace"];
  EXPECT_EQ(folly::toJson(inner["full_namespace"]), R"(["test","namespace"])");

  removeSession(publisherSession);
  exec_->drive();
}

TEST_P(MoQRelayStateTest, NamespaceRoutesAreListedBestFirst) {
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "", 900);
  auto costly = createMockSession();
  negotiateCluster(*costly);
  auto cheap = createMockSession();
  negotiateCluster(*cheap);
  doPublishNamespaceMsg(costly, clusterAdvert({100}, 5));
  doPublishNamespaceMsg(cheap, clusterAdvert({200}, 1));
  driveIfMultiThread();

  auto state = dumpState();

  const auto& node = state["namespace_tree"]["children"]["test"]["children"]["namespace"];
  const auto& routes = node["routes"];
  ASSERT_EQ(routes.size(), 2) << folly::toJson(node);
  EXPECT_EQ(folly::toJson(routes[0]["hop_path"]), "[200]");
  EXPECT_EQ(routes[0]["cost"].asInt(), 2); // 1 plus the default link cost
  EXPECT_TRUE(routes[0]["from_peering"].asBool());
  EXPECT_EQ(folly::toJson(routes[1]["hop_path"]), "[100]");
  EXPECT_EQ(routes[1]["cost"].asInt(), 6);
  EXPECT_EQ(node["publisher"], routes[0]["address"]);

  removeSession(costly);
  removeSession(cheap);
  exec_->drive();
}

INSTANTIATE_TEST_SUITE_P(
    AllModes,
    MoQRelayStateTest,
    ::testing::Values(RelayMode::SingleThread, RelayMode::MultiThread, RelayMode::LocalForwarderMT),
    [](const ::testing::TestParamInfo<RelayMode>& info) {
      std::ostringstream os;
      PrintTo(info.param, &os);
      return os.str();
    }
);

} // namespace openmoq::moqx::test
