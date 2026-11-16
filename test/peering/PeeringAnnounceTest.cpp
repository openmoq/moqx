/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MoqxRelayTestFixture.h"
#include "peering/MoqPeeringParticipant.h"
#include "peering/PeeringAnnouncer.h"
#include "peering/PeeringObject.h"

#include <folly/io/IOBuf.h>
#include <gmock/gmock.h>

#include <atomic>
#include <mutex>
#include <string>

namespace openmoq::moqx::test {

class RecordingAnnouncer : public PeeringAnnouncer {
public:
  RecordingAnnouncer() : session(makeSessionId()) {}

  void announceNamespace(const TrackNamespace& ns) override {
    std::lock_guard<std::mutex> lock(mu);
    namespaces.push_back(ns);
  }
  void withdrawNamespace(const TrackNamespace& ns) override {
    std::lock_guard<std::mutex> lock(mu);
    withdrawnNamespaces.push_back(ns);
  }
  void announceTrack(const FullTrackName& ftn) override {
    std::lock_guard<std::mutex> lock(mu);
    tracks.push_back(ftn);
  }
  void withdrawTrack(const FullTrackName& ftn) override {
    std::lock_guard<std::mutex> lock(mu);
    withdrawnTracks.push_back(ftn);
  }
  void sendObject(const FullTrackName& ftn, PeeringObject object) override {
    std::lock_guard<std::mutex> lock(mu);
    sent.push_back(SentObject{ftn, std::move(object)});
  }
  void subscribeTrack(const FullTrackName& ftn) override {
    std::lock_guard<std::mutex> lock(mu);
    subscribed.push_back(ftn);
  }
  void unsubscribeTrack(const FullTrackName& ftn) override {
    std::lock_guard<std::mutex> lock(mu);
    unsubscribed.push_back(ftn);
  }
  SessionId sessionId() const override { return session; }

  SessionId session;
  std::mutex mu;
  std::vector<TrackNamespace> namespaces;
  std::vector<TrackNamespace> withdrawnNamespaces;
  std::vector<FullTrackName> tracks;
  std::vector<FullTrackName> withdrawnTracks;

  struct SentObject {
    FullTrackName ftn;
    PeeringObject object;
  };
  std::vector<SentObject> sent;
  std::vector<FullTrackName> subscribed;
  std::vector<FullTrackName> unsubscribed;
};

Publisher::SubscribeResult acceptNamespaceSubscribe(SubscribeRequest sub) {
  SubscribeOk ok;
  ok.requestID = sub.requestID;
  ok.trackAlias = TrackAlias(0);
  ok.expires = std::chrono::milliseconds(0);
  ok.groupOrder = GroupOrder::OldestFirst;
  return std::static_pointer_cast<Publisher::SubscriptionHandle>(
      std::make_shared<NiceMock<MockSubscriptionHandle>>(std::move(ok))
  );
}

void allowPublisherSubscribe(MockMoQSession& session) {
  ON_CALL(session, subscribe(_, _)).WillByDefault([](SubscribeRequest sub, auto) {
    return folly::coro::makeTask<Publisher::SubscribeResult>(
        acceptNamespaceSubscribe(std::move(sub))
    );
  });
}

std::shared_ptr<MoqPeeringParticipant> makePeering(
    const std::shared_ptr<MoqxRelay>& relay,
    std::shared_ptr<RecordingAnnouncer> announcer
) {
  auto participant = std::make_shared<MoqPeeringParticipant>(std::move(announcer), relay);
  participant->attach();
  return participant;
}

TEST_P(MoQRelayTest, PeeringAnnouncesLocalPublishAndNamespace) {
  auto announcer = std::make_shared<RecordingAnnouncer>();
  auto peering = makePeering(relay_, announcer);
  ASSERT_TRUE(driveUntil([&] { return peering->ready(); }));
  auto first = createMockSession();
  auto second = createMockSession();

  auto firstHandle = doPublishNamespace(first, kTestNamespace);
  auto secondHandle = doPublishNamespace(second, kTestNamespace);
  auto consumer = doPublish(first, kTestTrackName);
  driveIfMultiThread();

  {
    std::lock_guard<std::mutex> lock(announcer->mu);
    ASSERT_EQ(announcer->namespaces.size(), 1u);
    EXPECT_EQ(announcer->namespaces[0], kTestNamespace);
    ASSERT_EQ(announcer->tracks.size(), 1u);
    EXPECT_EQ(announcer->tracks[0], kTestTrackName);
  }

  // A second publisher replaces the non-cluster route. The first done must not
  // withdraw the namespace while the second publisher remains.
  firstHandle->publishNamespaceDone();
  driveIfMultiThread();
  {
    std::lock_guard<std::mutex> lock(announcer->mu);
    EXPECT_TRUE(announcer->withdrawnNamespaces.empty());
  }

  secondHandle->publishNamespaceDone();
  ASSERT_TRUE(driveUntil([&] {
    std::lock_guard<std::mutex> lock(announcer->mu);
    return announcer->withdrawnNamespaces.size() == 1u;
  }));
  {
    std::lock_guard<std::mutex> lock(announcer->mu);
    EXPECT_EQ(announcer->withdrawnNamespaces[0], kTestNamespace);
  }

  consumer->publishDone(PublishDone{RequestID(0), PublishDoneStatusCode::TRACK_ENDED, 0, "done"});
  ASSERT_TRUE(driveUntil([&] {
    std::lock_guard<std::mutex> lock(announcer->mu);
    return announcer->withdrawnTracks.size() == 1u;
  }));
  {
    std::lock_guard<std::mutex> lock(announcer->mu);
    EXPECT_EQ(announcer->withdrawnTracks[0], kTestTrackName);
  }

  removeSession(first);
  removeSession(second);
}

TEST_P(MoQRelayTest, PeeringInjectsNamespaceAndTrack) {
  auto announcer = std::make_shared<RecordingAnnouncer>();
  auto peering = makePeering(relay_, announcer);
  ASSERT_TRUE(driveUntil([&] { return peering->ready(); }));

  verifyOnRelayExec([&] {
    peering->onNamespace(kTestNamespace, false);
    peering->onTrack(kTestTrackName, false);
  });

  std::optional<SessionId> namespacePublisher;
  bool trackPublished = false;
  for (int i = 0; i < (relayEvb_ ? 20 : 1) && !trackPublished; ++i) {
    if (relayEvb_) {
      exec_->drive();
    }
    verifyOnRelayExec([&] {
      namespacePublisher = relay_->findNamespacePublisher(kTestNamespace);
      auto track = relay_->findPublishState(kTestTrackName);
      trackPublished = track.publisher && *track.publisher == announcer->sessionId();
    });
  }
  ASSERT_TRUE(namespacePublisher.has_value());
  EXPECT_EQ(*namespacePublisher, announcer->sessionId());
  EXPECT_TRUE(trackPublished);
  {
    std::lock_guard<std::mutex> lock(announcer->mu);
    EXPECT_TRUE(announcer->namespaces.empty());
    EXPECT_TRUE(announcer->tracks.empty());
  }

  verifyOnRelayExec([&] {
    peering->onNamespace(kTestNamespace, true);
    peering->onTrack(kTestTrackName, true);
  });
  exec_->drive();
  verifyOnRelayExec([&] {
    EXPECT_TRUE(relay_->findNamespacePublisher(kTestNamespace).has_value());
    auto track = relay_->findPublishState(kTestTrackName);
    EXPECT_TRUE(track.publisher && *track.publisher == announcer->sessionId());
  });
}

// The new publish is announced before the old one is withdrawn. The withdraw
// must not remove the namespace or the track the new publish is using.
TEST_P(MoQRelayTest, PeeringAnnounceBeforeWithdrawKeepsTrack) {
  auto announcer = std::make_shared<RecordingAnnouncer>();
  auto peering = makePeering(relay_, announcer);
  ASSERT_TRUE(driveUntil([&] { return peering->ready(); }));
  verifyOnRelayExec([&] {
    peering->onNamespace(kTestNamespace, false);
    peering->onTrack(kTestTrackName, false);
  });
  ASSERT_TRUE(driveUntil([&] {
    bool published = false;
    verifyOnRelayExec([&] {
      auto track = relay_->findPublishState(kTestTrackName);
      published = track.publisher && *track.publisher == announcer->sessionId();
    });
    return published;
  }));

  verifyOnRelayExec([&] {
    peering->onNamespace(kTestNamespace, false);
    peering->onTrack(kTestTrackName, false);
    peering->onNamespace(kTestNamespace, true);
    peering->onTrack(kTestTrackName, true);
  });
  exec_->drive();
  verifyOnRelayExec([&] {
    auto track = relay_->findPublishState(kTestTrackName);
    EXPECT_TRUE(track.publisher && *track.publisher == announcer->sessionId());
    EXPECT_TRUE(relay_->findNamespacePublisher(kTestNamespace).has_value());
  });

  auto subscriber = createMockSession();
  auto mock = createMockConsumer();
  auto mockSg = createMockSubgroupConsumer();
  std::mutex gotMu;
  std::string got;
  EXPECT_CALL(*mock, beginSubgroup(9, 1, 4, testing::_))
      .WillOnce(
          testing::Return(
              folly::makeExpected<MoQPublishError>(
                  std::static_pointer_cast<SubgroupConsumer>(mockSg)
              )
          )
      );
  EXPECT_CALL(*mockSg, object(1, testing::_, testing::_, testing::_))
      .WillOnce(testing::Invoke([&](uint64_t, Payload payload, Extensions, bool) {
        std::string bytes;
        if (payload && payload->computeChainDataLength() > 0) {
          auto copy = payload->cloneCoalesced();
          bytes.assign(reinterpret_cast<const char*>(copy->data()), copy->length());
        }
        std::lock_guard<std::mutex> lock(gotMu);
        got = std::move(bytes);
        return folly::makeExpected<MoQPublishError>(folly::unit);
      }));
  subscribeToTrack(subscriber, kTestTrackName, mock, RequestID(1));
  ASSERT_TRUE(driveUntil([&] {
    std::lock_guard<std::mutex> lock(announcer->mu);
    return announcer->subscribed.size() == 1u;
  }));

  PeeringObject object;
  object.group = 9;
  object.subgroup = 1;
  object.objectId = 1;
  object.priority = 4;
  object.payload = {'n', 'e', 'x', 't'};
  PeeringSubgroupWriter writer(1);
  auto encoded = writer.write(object, true);
  ASSERT_TRUE(encoded.has_value());
  PeeringStreamReader reader;
  ASSERT_TRUE(reader.ingest(encoded->data(), encoded->size(), false));
  auto events = reader.take();
  verifyOnRelayExec([&] {
    for (const auto& event : events) {
      peering->acceptEvent(kTestTrackName, 40, event);
    }
  });
  ASSERT_TRUE(driveUntil([&] {
    std::lock_guard<std::mutex> lock(gotMu);
    return got == "next";
  }));
  removeSession(subscriber);
  exec_->drive();
}

// A subscriber can arrive once the peered namespace is published and before the
// track announce. That subscribe has to pull, and the later track inject must
// not tear the pull down.
TEST_P(MoQRelayTest, PeeringSubscribeBeforeTrackInjectPulls) {
  auto announcer = std::make_shared<RecordingAnnouncer>();
  auto peering = makePeering(relay_, announcer);
  ASSERT_TRUE(driveUntil([&] { return peering->ready(); }));
  verifyOnRelayExec([&] { peering->onNamespace(kTestNamespace, false); });
  ASSERT_TRUE(driveUntil([&] {
    bool published = false;
    verifyOnRelayExec([&] {
      auto publisher = relay_->findNamespacePublisher(kTestNamespace);
      published = publisher && *publisher == announcer->sessionId();
    });
    return published;
  }));

  auto subscriber = createMockSession();
  auto mock = createMockConsumer();
  ASSERT_NE(subscribeToTrack(subscriber, kTestTrackName, mock, RequestID(1)), nullptr);
  ASSERT_TRUE(driveUntil([&] {
    std::lock_guard<std::mutex> lock(announcer->mu);
    return announcer->subscribed.size() == 1u;
  }));

  verifyOnRelayExec([&] { peering->onTrack(kTestTrackName, false); });
  int turns = 0;
  driveUntil([&] { return ++turns > (relayEvb_ ? 8 : 0); });
  {
    std::lock_guard<std::mutex> lock(announcer->mu);
    EXPECT_TRUE(announcer->unsubscribed.empty());
    EXPECT_EQ(announcer->subscribed.size(), 1u);
  }
  removeSession(subscriber);
  exec_->drive();
}

// The subscriber connects before any publisher has announced. The subscribe has
// to pull immediately, and the later announce must not drop that pull.
TEST_P(MoQRelayTest, PeeringSubscribeBeforeAnnouncePulls) {
  auto announcer = std::make_shared<RecordingAnnouncer>();
  auto peering = makePeering(relay_, announcer);
  ASSERT_TRUE(driveUntil([&] { return peering->ready(); }));

  auto subscriber = createMockSession();
  auto mock = createMockConsumer();
  ASSERT_NE(subscribeToTrack(subscriber, kTestTrackName, mock, RequestID(1)), nullptr);
  ASSERT_TRUE(driveUntil([&] {
    std::lock_guard<std::mutex> lock(announcer->mu);
    return announcer->subscribed.size() == 1u;
  }));

  verifyOnRelayExec([&] {
    peering->onNamespace(kTestNamespace, false);
    peering->onTrack(kTestTrackName, false);
  });
  int turns = 0;
  driveUntil([&] { return ++turns > (relayEvb_ ? 8 : 0); });
  {
    std::lock_guard<std::mutex> lock(announcer->mu);
    EXPECT_TRUE(announcer->unsubscribed.empty());
    ASSERT_EQ(announcer->subscribed.size(), 1u);
    EXPECT_EQ(announcer->subscribed[0], kTestTrackName);
  }
  removeSession(subscriber);
  exec_->drive();
}

// A peered subscribe that matches a client's PUBLISH_NAMESPACE subscribes that
// track from the client. A track outside the namespace does not.
TEST_P(MoQRelayTest, PeeringNamespaceSubscribePullsPublisher) {
  auto announcer = std::make_shared<RecordingAnnouncer>();
  auto peering = makePeering(relay_, announcer);
  ASSERT_TRUE(driveUntil([&] { return peering->ready(); }));
  auto session = createMockSession();
  std::atomic<int> pulls{0};
  EXPECT_CALL(*session, subscribe(_, _))
      .WillOnce([&](SubscribeRequest sub, std::shared_ptr<TrackConsumer>) {
        EXPECT_EQ(sub.fullTrackName, kTestTrackName);
        EXPECT_TRUE(sub.forward);
        pulls.fetch_add(1);
        return folly::coro::makeTask<Publisher::SubscribeResult>(
            acceptNamespaceSubscribe(std::move(sub))
        );
      });
  doPublishNamespace(session, kTestNamespace);
  verifyOnRelayExec([&] { peering->onSubscribe(kTestTrackName); });
  ASSERT_TRUE(driveUntil([&] { return pulls.load() == 1; }));

  verifyOnRelayExec([&] {
    peering->onSubscribe(FullTrackName{TrackNamespace{{"other"}}, "track"});
  });
  int turns = 0;
  driveUntil([&] { return ++turns > (relayEvb_ ? 4 : 0); });
  EXPECT_EQ(pulls.load(), 1);
  removeSession(session);
  exec_->drive();
}

// The peered subscribe can arrive before the namespace. The later
// PUBLISH_NAMESPACE subscribes the matching track from that client.
TEST_P(MoQRelayTest, PeeringSubscribeBeforeNamespacePullsPublisher) {
  auto announcer = std::make_shared<RecordingAnnouncer>();
  auto peering = makePeering(relay_, announcer);
  ASSERT_TRUE(driveUntil([&] { return peering->ready(); }));
  verifyOnRelayExec([&] { peering->onSubscribe(kTestTrackName); });

  auto session = createMockSession();
  std::atomic<bool> pulled{false};
  EXPECT_CALL(*session, subscribe(_, _))
      .WillOnce([&](SubscribeRequest sub, std::shared_ptr<TrackConsumer>) {
        EXPECT_EQ(sub.fullTrackName, kTestTrackName);
        EXPECT_TRUE(sub.forward);
        pulled.store(true);
        return folly::coro::makeTask<Publisher::SubscribeResult>(
            acceptNamespaceSubscribe(std::move(sub))
        );
      });
  doPublishNamespace(session, kTestNamespace);
  ASSERT_TRUE(driveUntil([&] { return pulled.load(); }));
  removeSession(session);
  exec_->drive();
}

// A local SUBSCRIBE that matches a later PUBLISH_NAMESPACE subscribes the track
// from that client.
TEST_P(MoQRelayTest, PeeringLocalSubscribeBeforeNamespacePullsPublisher) {
  auto announcer = std::make_shared<RecordingAnnouncer>();
  auto peering = makePeering(relay_, announcer);
  ASSERT_TRUE(driveUntil([&] { return peering->ready(); }));

  auto subscriber = createMockSession();
  auto mock = createMockConsumer();
  ASSERT_NE(subscribeToTrack(subscriber, kTestTrackName, mock, RequestID(1)), nullptr);

  auto session = createMockSession();
  std::atomic<bool> pulled{false};
  EXPECT_CALL(*session, subscribe(_, _))
      .WillOnce([&](SubscribeRequest sub, std::shared_ptr<TrackConsumer>) {
        EXPECT_EQ(sub.fullTrackName, kTestTrackName);
        EXPECT_TRUE(sub.forward);
        pulled.store(true);
        return folly::coro::makeTask<Publisher::SubscribeResult>(
            acceptNamespaceSubscribe(std::move(sub))
        );
      });
  doPublishNamespace(session, kTestNamespace);
  ASSERT_TRUE(driveUntil([&] { return pulled.load(); }));
  removeSession(subscriber);
  removeSession(session);
  exec_->drive();
}

// The client rejects SUBSCRIBE until it has published the track. The relay
// sends it again once that PUBLISH arrives.
TEST_P(MoQRelayTest, PeeringNamespaceSubscribeRetriesAfterTrackPublish) {
  auto announcer = std::make_shared<RecordingAnnouncer>();
  auto peering = makePeering(relay_, announcer);
  ASSERT_TRUE(driveUntil([&] { return peering->ready(); }));
  verifyOnRelayExec([&] { peering->onSubscribe(kTestTrackName); });

  auto session = createMockSession();
  std::atomic<int> pulls{0};
  EXPECT_CALL(*session, subscribe(_, _))
      .WillOnce([&](SubscribeRequest sub, std::shared_ptr<TrackConsumer>) {
        EXPECT_EQ(sub.fullTrackName, kTestTrackName);
        pulls.fetch_add(1);
        return folly::coro::makeTask<Publisher::SubscribeResult>(folly::makeUnexpected(
            SubscribeError{sub.requestID, SubscribeErrorCode::DOES_NOT_EXIST, "Published track not found"}
        ));
      })
      .WillOnce([&](SubscribeRequest sub, std::shared_ptr<TrackConsumer>) {
        EXPECT_EQ(sub.fullTrackName, kTestTrackName);
        EXPECT_TRUE(sub.forward);
        pulls.fetch_add(1);
        return folly::coro::makeTask<Publisher::SubscribeResult>(
            acceptNamespaceSubscribe(std::move(sub))
        );
      });

  doPublishNamespace(session, kTestNamespace);
  ASSERT_TRUE(driveUntil([&] { return pulls.load() >= 1; }));
  doPublish(session, kTestTrackName);
  ASSERT_TRUE(driveUntil([&] { return pulls.load() == 2; }));
  removeSession(session);
  exec_->drive();
}

TEST_P(MoQRelayTest, PeeringSendsObjectsToSubscribedPeer) {
  auto announcer = std::make_shared<RecordingAnnouncer>();
  auto peering = makePeering(relay_, announcer);
  ASSERT_TRUE(driveUntil([&] { return peering->ready(); }));
  auto session = createMockSession();
  allowPublisherSubscribe(*session);
  doPublishNamespace(session, kTestNamespace);
  auto consumer = doPublish(session, kTestTrackName);
  verifyOnRelayExec([&] { peering->onSubscribe(kTestTrackName); });
  int turns = 0;
  ASSERT_TRUE(driveUntil([&] { return ++turns > (relayEvb_ ? 4 : 0); }));

  ObjectHeader header(9, 2, 4, /*priority=*/6, ObjectStatus::NORMAL);
  auto payload = folly::IOBuf::copyBuffer(std::string("hello-peer"));
  ASSERT_TRUE(consumer->objectStream(header, std::move(payload), true).hasValue());
  ASSERT_TRUE(driveUntil([&] {
    std::lock_guard<std::mutex> lock(announcer->mu);
    return announcer->sent.size() == 1u;
  }));
  {
    std::lock_guard<std::mutex> lock(announcer->mu);
    ASSERT_EQ(announcer->sent.size(), 1u);
    EXPECT_EQ(announcer->sent[0].ftn, kTestTrackName);
    const auto& object = announcer->sent[0].object;
    EXPECT_EQ(object.group, 9u);
    EXPECT_EQ(object.subgroup, 2u);
    EXPECT_EQ(object.objectId, 4u);
    EXPECT_EQ(object.priority, 6);
    EXPECT_TRUE(object.lastInGroup);
    EXPECT_FALSE(object.datagram);
    EXPECT_EQ(object.status, static_cast<uint8_t>(ObjectStatus::NORMAL));
    EXPECT_EQ(std::string(object.payload.begin(), object.payload.end()), "hello-peer");
  }
  removeSession(session);
}

// A publisher that published before anyone subscribed is paused. A later peer
// subscribe has to send REQUEST_UPDATE forward=true or it never writes objects.
TEST_P(MoQRelayTest, PeeringSubscribeResumesPausedPublisher) {
  auto announcer = std::make_shared<RecordingAnnouncer>();
  auto peering = makePeering(relay_, announcer);
  ASSERT_TRUE(driveUntil([&] { return peering->ready(); }));
  auto session = createMockSession();
  allowPublisherSubscribe(*session);
  doPublishNamespace(session, kTestNamespace);
  auto handle = makePublishHandle();
  doPublishWithHandle(session, kTestTrackName, handle);

  bool resumed = false;
  EXPECT_CALL(*handle, requestUpdateCalled(_)).WillOnce([&](const RequestUpdate& update) {
    resumed = update.forward.has_value() && *update.forward;
  });

  verifyOnRelayExec([&] { peering->onSubscribe(kTestTrackName); });
  ASSERT_TRUE(driveUntil([&] { return resumed; }));
  ASSERT_TRUE(Mock::VerifyAndClearExpectations(handle.get()));

  removeSession(session);
}

// A peer subscriber is already interested when the publisher starts. PUBLISH_OK
// has to carry forward=1, which the relay applies as a request update.
TEST_P(MoQRelayTest, PeeringSubscribeBeforePublisherResumes) {
  auto announcer = std::make_shared<RecordingAnnouncer>();
  auto peering = makePeering(relay_, announcer);
  ASSERT_TRUE(driveUntil([&] { return peering->ready(); }));
  verifyOnRelayExec([&] { peering->onSubscribe(kTestTrackName); });

  auto session = createMockSession();
  ON_CALL(*session, subscribe(_, _)).WillByDefault([](SubscribeRequest sub, auto) {
    return folly::coro::makeTask<Publisher::SubscribeResult>(
        acceptNamespaceSubscribe(std::move(sub))
    );
  });
  doPublishNamespace(session, kTestNamespace);
  auto handle = makePublishHandle();
  bool resumed = false;
  EXPECT_CALL(*handle, requestUpdateCalled(_)).WillOnce([&](const RequestUpdate& update) {
    resumed = update.forward.has_value() && *update.forward;
  });
  doPublishWithHandle(session, kTestTrackName, handle);
  ASSERT_TRUE(driveUntil([&] { return resumed; }));
  removeSession(session);
}

// A live subgroup used to hide the forward=0 update: removeSubscriber waits for
// the subgroup to end before onEmpty, and suppresses forwardChanged. The
// publisher then keeps sending after the last peer subscriber has left.
TEST_P(MoQRelayTest, PeeringUnsubscribePausesPublisher) {
  auto announcer = std::make_shared<RecordingAnnouncer>();
  auto peering = makePeering(relay_, announcer);
  ASSERT_TRUE(driveUntil([&] { return peering->ready(); }));
  auto session = createMockSession();
  allowPublisherSubscribe(*session);
  doPublishNamespace(session, kTestNamespace);
  auto handle = makePublishHandle();
  auto consumer = doPublishWithHandle(session, kTestTrackName, handle);
  ASSERT_NE(consumer, nullptr);

  bool forwarding = false;
  EXPECT_CALL(*handle, requestUpdateCalled(_)).WillOnce([&](const RequestUpdate& update) {
    forwarding = update.forward.has_value() && *update.forward;
  });
  verifyOnRelayExec([&] { peering->onSubscribe(kTestTrackName); });
  ASSERT_TRUE(driveUntil([&] { return forwarding; }));
  ASSERT_TRUE(Mock::VerifyAndClearExpectations(handle.get()));

  withSessionContext(session, [&] {
    auto opened = consumer->beginSubgroup(1, 0, 0);
    ASSERT_TRUE(opened.hasValue());
    auto wrote = opened.value()->object(0, folly::IOBuf::copyBuffer("x"));
    ASSERT_TRUE(wrote.hasValue());
  });

  bool paused = false;
  EXPECT_CALL(*handle, requestUpdateCalled(_)).WillOnce([&](const RequestUpdate& update) {
    paused = update.forward.has_value() && !*update.forward;
  });
  verifyOnRelayExec([&] { peering->onUnsubscribe(kTestTrackName); });
  ASSERT_TRUE(driveUntil([&] { return paused; }));
  removeSession(session);
}

// A directly connected client that is still forwarding keeps the publisher going
// after the last peering subscriber leaves.
TEST_P(MoQRelayTest, PeeringUnsubscribeKeepsForwardingClient) {
  auto announcer = std::make_shared<RecordingAnnouncer>();
  auto peering = makePeering(relay_, announcer);
  ASSERT_TRUE(driveUntil([&] { return peering->ready(); }));
  auto session = createMockSession();
  allowPublisherSubscribe(*session);
  doPublishNamespace(session, kTestNamespace);
  auto handle = makePublishHandle();
  ASSERT_NE(doPublishWithHandle(session, kTestTrackName, handle), nullptr);

  auto subscriber = createMockSession();
  auto mock = createMockConsumer();
  EXPECT_CALL(*mock, beginSubgroup(_, _, _, _))
      .WillRepeatedly(Return(
          folly::makeExpected<MoQPublishError>(
              std::static_pointer_cast<SubgroupConsumer>(createMockSubgroupConsumer())
          )
      ));
  bool forwarding = false;
  EXPECT_CALL(*handle, requestUpdateCalled(_)).WillRepeatedly([&](const RequestUpdate& update) {
    if (update.forward.has_value() && *update.forward) {
      forwarding = true;
    }
  });
  subscribeToTrack(subscriber, kTestTrackName, mock, RequestID(1));
  ASSERT_TRUE(driveUntil([&] { return forwarding; }));
  ASSERT_TRUE(Mock::VerifyAndClearExpectations(handle.get()));

  bool paused = false;
  EXPECT_CALL(*handle, requestUpdateCalled(_)).WillRepeatedly([&](const RequestUpdate& update) {
    if (update.forward.has_value() && !*update.forward) {
      paused = true;
    }
  });
  verifyOnRelayExec([&] {
    peering->onSubscribe(kTestTrackName);
    peering->onUnsubscribe(kTestTrackName);
  });
  driveUntil([&] { return paused; }, 20);
  EXPECT_FALSE(paused);
  removeSession(subscriber);
  removeSession(session);
  // The last local subscriber leaving queues a publisher update. Run it before
  // the mock handle is destroyed with the test.
  exec_->drive();
}

TEST_P(MoQRelayTest, PeeringDeliversObjectsToLocalSubscriber) {
  auto announcer = std::make_shared<RecordingAnnouncer>();
  auto peering = makePeering(relay_, announcer);
  ASSERT_TRUE(driveUntil([&] { return peering->ready(); }));
  verifyOnRelayExec([&] {
    peering->onNamespace(kTestNamespace, false);
    peering->onTrack(kTestTrackName, false);
  });
  bool published = false;
  for (int i = 0; i < (relayEvb_ ? 20 : 1) && !published; ++i) {
    if (relayEvb_) {
      exec_->drive();
    }
    verifyOnRelayExec([&] {
      auto track = relay_->findPublishState(kTestTrackName);
      published = track.publisher && *track.publisher == announcer->sessionId();
    });
  }
  ASSERT_TRUE(published);

  auto subscriber = createMockSession();
  auto mock = createMockConsumer();
  auto mockSg = createMockSubgroupConsumer();
  std::mutex gotMu;
  std::string got;
  EXPECT_CALL(*mock, beginSubgroup(3, 1, 4, testing::_))
      .WillOnce(
          testing::Return(
              folly::makeExpected<MoQPublishError>(
                  std::static_pointer_cast<SubgroupConsumer>(mockSg)
              )
          )
      );
  EXPECT_CALL(*mockSg, object(8, testing::_, testing::_, testing::_))
      .WillOnce(testing::Invoke([&](uint64_t, Payload payload, Extensions, bool) {
        std::string bytes;
        if (payload && payload->computeChainDataLength() > 0) {
          auto copy = payload->cloneCoalesced();
          bytes.assign(reinterpret_cast<const char*>(copy->data()), copy->length());
        }
        std::lock_guard<std::mutex> lock(gotMu);
        got = std::move(bytes);
        return folly::makeExpected<MoQPublishError>(folly::unit);
      }));
  EXPECT_CALL(*mockSg, endOfSubgroup())
      .WillRepeatedly(testing::Return(folly::makeExpected<MoQPublishError>(folly::unit)));
  subscribeToTrack(subscriber, kTestTrackName, mock, RequestID(1));
  ASSERT_TRUE(driveUntil([&] {
    std::lock_guard<std::mutex> lock(announcer->mu);
    return !announcer->subscribed.empty();
  }));

  PeeringObject object;
  object.group = 3;
  object.subgroup = 1;
  object.objectId = 8;
  object.priority = 4;
  object.payload = {'p', 'i', 'n', 'g'};
  PeeringSubgroupWriter writer(1);
  auto encoded = writer.write(object, true);
  ASSERT_TRUE(encoded.has_value());
  PeeringStreamReader reader;
  ASSERT_TRUE(reader.ingest(encoded->data(), encoded->size(), true));
  auto events = reader.take();
  verifyOnRelayExec([&] {
    for (const auto& event : events) {
      peering->acceptEvent(kTestTrackName, 11, event);
    }
  });
  ASSERT_TRUE(driveUntil([&] {
    std::lock_guard<std::mutex> lock(gotMu);
    return got.size() == 4u;
  }));
  {
    std::lock_guard<std::mutex> lock(gotMu);
    EXPECT_EQ(got, "ping");
  }
  {
    std::lock_guard<std::mutex> lock(announcer->mu);
    EXPECT_TRUE(announcer->sent.empty());
    ASSERT_EQ(announcer->subscribed.size(), 1u);
    EXPECT_EQ(announcer->subscribed[0], kTestTrackName);
  }
  removeSession(subscriber);
}

// The peered publisher keeps the same subgroup after the local subscriber
// leaves and comes back. The objects that arrived while nobody was listening
// reset that subgroup, so the next object has to open it again.
TEST_P(MoQRelayTest, PeeringResubscribeReceivesOpenSubgroup) {
  auto announcer = std::make_shared<RecordingAnnouncer>();
  auto peering = makePeering(relay_, announcer);
  ASSERT_TRUE(driveUntil([&] { return peering->ready(); }));
  verifyOnRelayExec([&] {
    peering->onNamespace(kTestNamespace, false);
    peering->onTrack(kTestTrackName, false);
  });
  bool published = false;
  for (int i = 0; i < (relayEvb_ ? 20 : 1) && !published; ++i) {
    if (relayEvb_) {
      exec_->drive();
    }
    verifyOnRelayExec([&] {
      auto track = relay_->findPublishState(kTestTrackName);
      published = track.publisher && *track.publisher == announcer->sessionId();
    });
  }
  ASSERT_TRUE(published);

  auto subscriber = createMockSession();
  auto mock = createMockConsumer();
  auto mockSg = createMockSubgroupConsumer();
  EXPECT_CALL(*mock, beginSubgroup(3, 1, 4, testing::_))
      .WillOnce(
          testing::Return(
              folly::makeExpected<MoQPublishError>(
                  std::static_pointer_cast<SubgroupConsumer>(mockSg)
              )
          )
      );
  EXPECT_CALL(*mockSg, object(testing::_, testing::_, testing::_, testing::_))
      .WillRepeatedly(testing::Return(folly::makeExpected<MoQPublishError>(folly::unit)));
  EXPECT_CALL(*mockSg, endOfSubgroup())
      .WillRepeatedly(testing::Return(folly::makeExpected<MoQPublishError>(folly::unit)));
  EXPECT_CALL(*mockSg, reset(testing::_)).WillRepeatedly(testing::Return());
  subscribeToTrack(subscriber, kTestTrackName, mock, RequestID(1));
  ASSERT_TRUE(driveUntil([&] {
    std::lock_guard<std::mutex> lock(announcer->mu);
    return announcer->subscribed.size() == 1u;
  }));

  PeeringSubgroupWriter writer(1);
  PeeringStreamReader reader;
  auto deliver = [&](uint64_t objectId, std::string bytes, bool start) {
    PeeringObject object;
    object.group = 3;
    object.subgroup = 1;
    object.objectId = objectId;
    object.priority = 4;
    object.payload.assign(bytes.begin(), bytes.end());
    auto encoded = writer.write(object, start);
    if (!encoded || !reader.ingest(encoded->data(), encoded->size(), false)) {
      ADD_FAILURE() << "subgroup object did not encode";
      return;
    }
    auto events = reader.take();
    verifyOnRelayExec([&] {
      for (const auto& event : events) {
        peering->acceptEvent(kTestTrackName, 11, event);
      }
    });
  };
  deliver(8, "ping", true);
  exec_->drive();

  removeSession(subscriber);
  ASSERT_TRUE(driveUntil([&] {
    std::lock_guard<std::mutex> lock(announcer->mu);
    return announcer->unsubscribed.size() == 1u;
  }));
  // One more object while nobody is forwarding. The relay drops the subgroup.
  deliver(9, "gap", false);
  exec_->drive();

  auto again = createMockSession();
  auto mockAgain = createMockConsumer();
  auto sgAgain = createMockSubgroupConsumer();
  std::mutex gotMu;
  std::string got;
  EXPECT_CALL(*mockAgain, beginSubgroup(3, 1, 4, testing::_))
      .WillOnce(
          testing::Return(
              folly::makeExpected<MoQPublishError>(
                  std::static_pointer_cast<SubgroupConsumer>(sgAgain)
              )
          )
      );
  EXPECT_CALL(*sgAgain, object(10, testing::_, testing::_, testing::_))
      .WillOnce(testing::Invoke([&](uint64_t, Payload payload, Extensions, bool) {
        std::string bytes;
        if (payload && payload->computeChainDataLength() > 0) {
          auto copy = payload->cloneCoalesced();
          bytes.assign(reinterpret_cast<const char*>(copy->data()), copy->length());
        }
        std::lock_guard<std::mutex> lock(gotMu);
        got = std::move(bytes);
        return folly::makeExpected<MoQPublishError>(folly::unit);
      }));
  subscribeToTrack(again, kTestTrackName, mockAgain, RequestID(2));
  ASSERT_TRUE(driveUntil([&] {
    std::lock_guard<std::mutex> lock(announcer->mu);
    return announcer->subscribed.size() == 2u;
  }));
  deliver(10, "pong", false);
  ASSERT_TRUE(driveUntil([&] {
    std::lock_guard<std::mutex> lock(gotMu);
    return got.size() == 4u;
  }));
  {
    std::lock_guard<std::mutex> lock(gotMu);
    EXPECT_EQ(got, "pong");
  }
  removeSession(again);
  exec_->drive();
}

// The publisher opens the subgroup again on a new stream. An object still
// arriving on the previous stream must not open the subgroup over the new one.
TEST_P(MoQRelayTest, PeeringOldStreamDoesNotResetNewSubgroup) {
  auto announcer = std::make_shared<RecordingAnnouncer>();
  auto peering = makePeering(relay_, announcer);
  ASSERT_TRUE(driveUntil([&] { return peering->ready(); }));
  verifyOnRelayExec([&] {
    peering->onNamespace(kTestNamespace, false);
    peering->onTrack(kTestTrackName, false);
  });
  bool published = false;
  for (int i = 0; i < (relayEvb_ ? 20 : 1) && !published; ++i) {
    if (relayEvb_) {
      exec_->drive();
    }
    verifyOnRelayExec([&] {
      auto track = relay_->findPublishState(kTestTrackName);
      published = track.publisher && *track.publisher == announcer->sessionId();
    });
  }
  ASSERT_TRUE(published);

  auto subscriber = createMockSession();
  auto mock = createMockConsumer();
  auto mockSg = createMockSubgroupConsumer();
  std::atomic<int> opened{0};
  EXPECT_CALL(*mock, beginSubgroup(3, 1, 4, testing::_))
      .WillRepeatedly(
          testing::DoAll(
              testing::InvokeWithoutArgs([&] { opened.fetch_add(1); }),
              testing::Return(
                  folly::makeExpected<MoQPublishError>(
                      std::static_pointer_cast<SubgroupConsumer>(mockSg)
                  )
              )
          )
      );
  std::mutex gotMu;
  std::string got;
  EXPECT_CALL(*mockSg, object(testing::_, testing::_, testing::_, testing::_))
      .WillRepeatedly(testing::Invoke([&](uint64_t, Payload payload, Extensions, bool) {
        std::string bytes;
        if (payload && payload->computeChainDataLength() > 0) {
          auto copy = payload->cloneCoalesced();
          bytes.assign(reinterpret_cast<const char*>(copy->data()), copy->length());
        }
        std::lock_guard<std::mutex> lock(gotMu);
        got = std::move(bytes);
        return folly::makeExpected<MoQPublishError>(folly::unit);
      }));
  EXPECT_CALL(*mockSg, endOfSubgroup())
      .WillRepeatedly(testing::Return(folly::makeExpected<MoQPublishError>(folly::unit)));
  EXPECT_CALL(*mockSg, reset(testing::_)).WillRepeatedly(testing::Return());
  subscribeToTrack(subscriber, kTestTrackName, mock, RequestID(1));
  ASSERT_TRUE(driveUntil([&] {
    std::lock_guard<std::mutex> lock(announcer->mu);
    return announcer->subscribed.size() == 1u;
  }));

  auto deliver = [&](uint64_t streamId,
                     PeeringSubgroupWriter& writer,
                     PeeringStreamReader& reader,
                     uint64_t objectId,
                     std::string bytes,
                     bool start) {
    PeeringObject object;
    object.group = 3;
    object.subgroup = 1;
    object.objectId = objectId;
    object.priority = 4;
    object.payload.assign(bytes.begin(), bytes.end());
    auto encoded = writer.write(object, start);
    if (!encoded || !reader.ingest(encoded->data(), encoded->size(), false)) {
      ADD_FAILURE() << "subgroup object did not encode";
      return;
    }
    auto events = reader.take();
    verifyOnRelayExec([&] {
      for (const auto& event : events) {
        peering->acceptEvent(kTestTrackName, streamId, event);
      }
    });
  };
  PeeringSubgroupWriter firstWriter(1);
  PeeringStreamReader firstReader;
  PeeringSubgroupWriter nextWriter(1);
  PeeringStreamReader nextReader;
  deliver(11, firstWriter, firstReader, 1, "old1", true);
  exec_->drive();
  deliver(22, nextWriter, nextReader, 1, "new1", true);
  ASSERT_TRUE(driveUntil([&] { return opened.load() == 2; }));
  deliver(11, firstWriter, firstReader, 2, "late", false);
  exec_->drive();
  EXPECT_EQ(opened.load(), 2);
  deliver(22, nextWriter, nextReader, 2, "live", false);
  ASSERT_TRUE(driveUntil([&] {
    std::lock_guard<std::mutex> lock(gotMu);
    return got == "live";
  }));
  removeSession(subscriber);
  exec_->drive();
}

// Data is still arriving when the last local subscriber disconnects. The open
// subgroup must not hide forward=0, or the peered publisher keeps sending.
TEST_P(MoQRelayTest, PeeringOpenSubgroupDisconnectUnpulls) {
  auto announcer = std::make_shared<RecordingAnnouncer>();
  auto peering = makePeering(relay_, announcer);
  ASSERT_TRUE(driveUntil([&] { return peering->ready(); }));
  verifyOnRelayExec([&] {
    peering->onNamespace(kTestNamespace, false);
    peering->onTrack(kTestTrackName, false);
  });
  bool published = false;
  for (int i = 0; i < (relayEvb_ ? 20 : 1) && !published; ++i) {
    if (relayEvb_) {
      exec_->drive();
    }
    verifyOnRelayExec([&] {
      auto track = relay_->findPublishState(kTestTrackName);
      published = track.publisher && *track.publisher == announcer->sessionId();
    });
  }
  ASSERT_TRUE(published);

  auto subscriber = createMockSession();
  auto mock = createMockConsumer();
  auto mockSg = createMockSubgroupConsumer();
  std::atomic<bool> opened{false};
  EXPECT_CALL(*mock, beginSubgroup(3, 1, 4, testing::_))
      .WillOnce(
          testing::DoAll(
              testing::InvokeWithoutArgs([&] { opened = true; }),
              testing::Return(
                  folly::makeExpected<MoQPublishError>(
                      std::static_pointer_cast<SubgroupConsumer>(mockSg)
                  )
              )
          )
      );
  subscribeToTrack(subscriber, kTestTrackName, mock, RequestID(1));
  ASSERT_TRUE(driveUntil([&] {
    std::lock_guard<std::mutex> lock(announcer->mu);
    return announcer->subscribed.size() == 1u;
  }));

  PeeringStreamReader::Event header;
  header.kind = PeeringStreamReader::Event::Kind::Subgroup;
  header.object.group = 3;
  header.object.subgroup = 1;
  header.object.priority = 4;
  verifyOnRelayExec([&] { peering->acceptEvent(kTestTrackName, 11, header); });
  ASSERT_TRUE(driveUntil([&] { return opened.load(); }));

  removeSession(subscriber);
  ASSERT_TRUE(driveUntil([&] {
    std::lock_guard<std::mutex> lock(announcer->mu);
    return announcer->unsubscribed.size() == 1u;
  }));

  // The subgroup is still open, so the next subscriber joins that forwarder.
  // Disconnecting them has to pause the publisher again.
  auto subscriber2 = createMockSession();
  auto mock2 = createMockConsumer();
  auto mockSg2 = createMockSubgroupConsumer();
  ON_CALL(*mock2, beginSubgroup(testing::_, testing::_, testing::_, testing::_))
      .WillByDefault(
          testing::Return(
              folly::makeExpected<MoQPublishError>(
                  std::static_pointer_cast<SubgroupConsumer>(mockSg2)
              )
          )
      );
  subscribeToTrack(subscriber2, kTestTrackName, mock2, RequestID(2));
  ASSERT_TRUE(driveUntil([&] {
    std::lock_guard<std::mutex> lock(announcer->mu);
    return announcer->subscribed.size() == 2u;
  }));
  removeSession(subscriber2);
  ASSERT_TRUE(driveUntil([&] {
    std::lock_guard<std::mutex> lock(announcer->mu);
    return announcer->unsubscribed.size() == 2u;
  }));
  if (exec_) {
    exec_->drive();
  }
}

} // namespace openmoq::moqx::test
