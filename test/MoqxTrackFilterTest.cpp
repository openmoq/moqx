/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * In-process integration tests for TRACK_FILTER (top-N track selection).
 *
 * All tests use NiceMock<MockMoQSession> driven by TestMoQExecutor::driveFor().
 * Verification is done by counting session->publish() calls per FullTrackName.
 */

#include "MoqxRelay.h"
#include <folly/coro/BlockingWait.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/Request.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <moxygen/MoQTrackProperties.h>
#include <moxygen/events/MoQFollyExecutorImpl.h>
#include <moxygen/relay/MoQForwarder.h>
#include <moxygen/test/MockMoQSession.h>
#include <moxygen/test/Mocks.h>

using namespace testing;
using namespace moxygen;
using namespace moxygen::test;
using namespace openmoq::moqx;

namespace {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

const TrackNamespace kNs{{"conf", "room1"}};
const TrackNamespace kPrefix{{"conf"}};
constexpr uint64_t kPropType = 0x100; // audio level property type

// ---------------------------------------------------------------------------
// Test executor (same pattern as MoqxRelayTest)
// ---------------------------------------------------------------------------

class TestMoQExecutor : public MoQFollyExecutorImpl, public folly::DrivableExecutor {
 public:
  explicit TestMoQExecutor() : MoQFollyExecutorImpl(&evb_) {}

  void add(folly::Func func) override {
    MoQFollyExecutorImpl::add(std::move(func));
  }

  void drive() override {
    if (auto* evb = getBackingEventBase()) {
      evb->loopOnce();
    }
  }

  void driveFor(int n) {
    for (int i = 0; i < n; i++) {
      drive();
    }
  }

 private:
  folly::EventBase evb_;
};

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class MoqxTrackFilterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    exec_ = std::make_shared<TestMoQExecutor>();
    relay_ = std::make_shared<MoqxRelay>(
        config::CacheConfig{0, 0}, // no cache
        /*relayID=*/"",
        /*maxDeselected=*/5);
    relay_->setAllowedNamespacePrefix(kPrefix);
  }

  void TearDown() override {
    relay_.reset();
    exec_->driveFor(10);
  }

  // Create a mock session wired to accept publish() calls.
  // publish() calls are recorded in publishedTracks_[session.get()].
  std::shared_ptr<MockMoQSession> makeSession() {
    auto session = std::make_shared<NiceMock<MockMoQSession>>(exec_);
    ON_CALL(*session, getNegotiatedVersion())
        .WillByDefault(Return(std::optional<uint64_t>(kVersionDraftCurrent)));

    auto* raw = session.get();
    ON_CALL(*session, publish(_, _))
        .WillByDefault(Invoke([this, raw](PublishRequest pub, auto) -> Subscriber::PublishResult {
          publishedTracks_[raw].push_back(pub.fullTrackName);
          auto consumer = std::make_shared<NiceMock<MockTrackConsumer>>();
          ON_CALL(*consumer, setTrackAlias(_))
              .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
          ON_CALL(*consumer, objectStream(_, _, _))
              .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
          ON_CALL(*consumer, publishDone(_))
              .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
          PublishOk ok{pub.requestID, true, 128, GroupOrder::Default,
                       LocationType::LargestObject, std::nullopt, std::make_optional(uint64_t(0))};
          return Subscriber::PublishConsumerAndReplyTask{
              std::static_pointer_cast<TrackConsumer>(consumer),
              folly::coro::makeTask<folly::Expected<PublishOk, PublishError>>(std::move(ok))};
        }));
    return session;
  }

  // How many times session received a publish() for ftn
  int publishCount(MockMoQSession* s, const FullTrackName& ftn) const {
    auto it = publishedTracks_.find(s);
    if (it == publishedTracks_.end()) {
      return 0;
    }
    return std::count(it->second.begin(), it->second.end(), ftn);
  }

  // All FTNs published to this session (in order)
  std::vector<FullTrackName> published(MockMoQSession* s) const {
    auto it = publishedTracks_.find(s);
    return it != publishedTracks_.end() ? it->second : std::vector<FullTrackName>{};
  }

  // ---- Relay helpers -------------------------------------------------------

  template <typename F>
  auto withSession(std::shared_ptr<MoQSession> session, F&& f) {
    folly::RequestContextScopeGuard guard;
    folly::RequestContext::get()->setContextData(
        sessionToken_,
        std::make_unique<MoQSession::MoQSessionRequestData>(std::move(session)));
    return f();
  }

  void doPublishNamespace(std::shared_ptr<MoQSession> session) {
    PublishNamespace pub;
    pub.trackNamespace = kNs;
    pub.requestID = RequestID(nextId_++);
    withSession(session, [&] {
      auto result = folly::coro::blockingWait(
          relay_->publishNamespace(std::move(pub), nullptr), exec_.get());
      ASSERT_TRUE(result.hasValue());
    });
  }

  // Publish a track; returns the TrackConsumer so caller can send objects.
  std::shared_ptr<TrackConsumer> doPublish(
      std::shared_ptr<MoQSession> session,
      const std::string& trackName,
      uint64_t initialLevel = 0) {
    PublishRequest pub;
    pub.fullTrackName = FullTrackName{kNs, trackName};
    pub.requestID = RequestID(nextId_++);
    pub.groupOrder = GroupOrder::OldestFirst;
    if (initialLevel > 0) {
      pub.extensions.insertMutableExtension(Extension{kPropType, initialLevel});
    }
    auto handle = makeHandle();
    std::shared_ptr<TrackConsumer> consumer;
    withSession(session, [&] {
      auto result = relay_->publish(std::move(pub), handle);
      ASSERT_TRUE(result.hasValue());
      consumer = result->consumer;
    });
    return consumer;
  }

  // Subscribe with TRACK_FILTER; returns the subNs handle.
  std::shared_ptr<Publisher::SubscribeNamespaceHandle> doSubscribeFilter(
      std::shared_ptr<MoQSession> session,
      uint64_t maxSelected) {
    SubscribeNamespace subNs;
    subNs.trackNamespacePrefix = kNs;
    subNs.requestID = RequestID(nextId_++);
    subNs.options = SubscribeNamespaceOptions::BOTH;
    subNs.forward = true;

    TrackRequestParameter p;
    p.key = folly::to_underlying(TrackRequestParamKey::TRACK_FILTER);
    p.asTrackFilter = TrackFilter{kPropType, maxSelected};
    subNs.params.insertParam(p);

    std::shared_ptr<Publisher::SubscribeNamespaceHandle> handle;
    withSession(session, [&] {
      auto result = folly::coro::blockingWait(
          relay_->subscribeNamespace(std::move(subNs), nullptr), exec_.get());
      ASSERT_TRUE(result.hasValue());
      handle = *result;
    });
    return handle;
  }

  // Send an object carrying a new property value
  void sendValue(std::shared_ptr<TrackConsumer> consumer, uint64_t value) {
    ObjectHeader hdr;
    hdr.group = 0;
    hdr.id = nextObjId_++;
    hdr.extensions.insertMutableExtension(Extension{kPropType, value});
    consumer->objectStream(hdr, nullptr, false); // errors OK (no subscribers yet)
  }

  std::shared_ptr<Publisher::SubscriptionHandle> makeHandle() {
    SubscribeOk ok;
    ok.requestID = RequestID(0);
    ok.trackAlias = TrackAlias(0);
    ok.expires = std::chrono::milliseconds(0);
    ok.groupOrder = GroupOrder::Default;
    return std::make_shared<NiceMock<MockSubscriptionHandle>>(std::move(ok));
  }

  FullTrackName ftn(const std::string& name) const {
    return FullTrackName{kNs, name};
  }

  // Subscribe without TRACK_FILTER (plain namespace subscription)
  void doSubscribeDirect(std::shared_ptr<MoQSession> session) {
    SubscribeNamespace subNs;
    subNs.trackNamespacePrefix = kNs;
    subNs.requestID = RequestID(nextId_++);
    subNs.options = SubscribeNamespaceOptions::BOTH;
    subNs.forward = true;
    withSession(session, [&] {
      auto result = folly::coro::blockingWait(
          relay_->subscribeNamespace(std::move(subNs), nullptr), exec_.get());
      ASSERT_TRUE(result.hasValue());
    });
  }

  PublishDone makePublishDone() {
    PublishDone done;
    done.requestID = RequestID(0);
    done.statusCode = PublishDoneStatusCode::TRACK_ENDED;
    return done;
  }

  // ---- Members -------------------------------------------------------------

  std::shared_ptr<TestMoQExecutor> exec_;
  std::shared_ptr<MoqxRelay> relay_;
  uint64_t nextId_{1};
  uint64_t nextObjId_{0};
  folly::RequestToken sessionToken_{"moq_session"};

  // per-session record of which FTNs were pushed via publish()
  std::map<MoQSession*, std::vector<FullTrackName>> publishedTracks_;
};

// ---------------------------------------------------------------------------
// Tests: subscriber joins before publishers
// ---------------------------------------------------------------------------

// Subscriber joins first; only top-N publishers should be forwarded.
TEST_F(MoqxTrackFilterTest, SubscribeFirst_Top2Of3_CorrectTracksForwarded) {
  auto pubSess = makeSession();
  doPublishNamespace(pubSess);

  auto viewer = makeSession();
  auto subHandle = doSubscribeFilter(viewer, /*maxSelected=*/2);
  ASSERT_NE(subHandle, nullptr);

  // Publish 3 tracks with different levels; top-2 are "a" (100) and "b" (80)
  auto cA = doPublish(pubSess, "a", 100);
  auto cB = doPublish(pubSess, "b", 80);
  auto cC = doPublish(pubSess, "c", 40);
  exec_->driveFor(20);

  EXPECT_EQ(publishCount(viewer.get(), ftn("a")), 1);
  EXPECT_EQ(publishCount(viewer.get(), ftn("b")), 1);
  EXPECT_EQ(publishCount(viewer.get(), ftn("c")), 0); // outside top-2

  cA->publishDone(makePublishDone());
  cB->publishDone(makePublishDone());
  cC->publishDone(makePublishDone());
}

// Top-1: only the single highest track reaches the subscriber.
TEST_F(MoqxTrackFilterTest, SubscribeFirst_Top1_OnlyHighestForwarded) {
  auto pubSess = makeSession();
  doPublishNamespace(pubSess);

  auto viewer = makeSession();
  doSubscribeFilter(viewer, /*maxSelected=*/1);

  auto cLoud = doPublish(pubSess, "loud", 100);
  auto cQuiet = doPublish(pubSess, "quiet", 30);
  exec_->driveFor(20);

  EXPECT_EQ(publishCount(viewer.get(), ftn("loud")), 1);
  EXPECT_EQ(publishCount(viewer.get(), ftn("quiet")), 0);

  cLoud->publishDone(makePublishDone());
  cQuiet->publishDone(makePublishDone());
}

// ---------------------------------------------------------------------------
// Tests: publisher joins before subscriber (late subscriber)
// ---------------------------------------------------------------------------

// Late subscriber should receive top-N from already-published tracks via
// getOrCreateRanking retroactive registration + addSessionToTopNGroup.
TEST_F(MoqxTrackFilterTest, PublishFirst_LateSubscriber_GetsTopN) {
  auto pubSess = makeSession();
  doPublishNamespace(pubSess);

  auto cA = doPublish(pubSess, "a", 100);
  auto cB = doPublish(pubSess, "b", 80);
  auto cC = doPublish(pubSess, "c", 40);
  exec_->driveFor(10);

  // Late subscriber joins; expects to immediately receive top-2
  auto viewer = makeSession();
  auto subHandle = doSubscribeFilter(viewer, /*maxSelected=*/2);
  ASSERT_NE(subHandle, nullptr);
  exec_->driveFor(20);

  EXPECT_EQ(publishCount(viewer.get(), ftn("a")), 1);
  EXPECT_EQ(publishCount(viewer.get(), ftn("b")), 1);
  EXPECT_EQ(publishCount(viewer.get(), ftn("c")), 0);

  cA->publishDone(makePublishDone());
  cB->publishDone(makePublishDone());
  cC->publishDone(makePublishDone());
}

// ---------------------------------------------------------------------------
// Tests: dynamic value changes
// ---------------------------------------------------------------------------

// A track outside top-N rises above the threshold and displaces the occupant.
TEST_F(MoqxTrackFilterTest, ValueChange_OutsiderEntersTopN) {
  auto pubSess = makeSession();
  doPublishNamespace(pubSess);

  auto viewer = makeSession();
  doSubscribeFilter(viewer, /*maxSelected=*/1);

  // "a" starts loudest, "b" starts quiet
  auto consumerA = doPublish(pubSess, "a", 100);
  auto consumerB = doPublish(pubSess, "b", 20);
  exec_->driveFor(20);

  ASSERT_EQ(publishCount(viewer.get(), ftn("a")), 1);
  ASSERT_EQ(publishCount(viewer.get(), ftn("b")), 0);

  // "b" jumps above "a"
  sendValue(consumerB, 110);
  exec_->driveFor(20);

  // viewer should now receive "b" (entered top-1)
  EXPECT_EQ(publishCount(viewer.get(), ftn("b")), 1);

  consumerA->publishDone(makePublishDone());
  consumerB->publishDone(makePublishDone());
}

// Two subscribers with different N values see independent top-N sets.
TEST_F(MoqxTrackFilterTest, TwoSubscribers_DifferentN_IndependentSets) {
  auto pubSess = makeSession();
  doPublishNamespace(pubSess);

  auto viewerTop1 = makeSession();
  auto viewerTop3 = makeSession();
  doSubscribeFilter(viewerTop1, 1);
  doSubscribeFilter(viewerTop3, 3);

  auto cA = doPublish(pubSess, "a", 100);
  auto cB = doPublish(pubSess, "b", 80);
  auto cC = doPublish(pubSess, "c", 60);
  auto cD = doPublish(pubSess, "d", 20);
  exec_->driveFor(20);

  // top-1 viewer: only "a"
  EXPECT_EQ(publishCount(viewerTop1.get(), ftn("a")), 1);
  EXPECT_EQ(publishCount(viewerTop1.get(), ftn("b")), 0);
  EXPECT_EQ(publishCount(viewerTop1.get(), ftn("c")), 0);

  // top-3 viewer: "a", "b", "c"
  EXPECT_EQ(publishCount(viewerTop3.get(), ftn("a")), 1);
  EXPECT_EQ(publishCount(viewerTop3.get(), ftn("b")), 1);
  EXPECT_EQ(publishCount(viewerTop3.get(), ftn("c")), 1);
  EXPECT_EQ(publishCount(viewerTop3.get(), ftn("d")), 0);

  cA->publishDone(makePublishDone());
  cB->publishDone(makePublishDone());
  cC->publishDone(makePublishDone());
  cD->publishDone(makePublishDone());
}

// ---------------------------------------------------------------------------
// Tests: forward flag
// ---------------------------------------------------------------------------

// forward=false propagates through to publishToSession without affecting selection.
// The subscriber should still receive publishes for selected tracks, but with
// forward=false passed to the forwarder (controls whether the relay requests
// forwarding from upstream).
TEST_F(MoqxTrackFilterTest, ForwardFalse_TracksStillSelected) {
  auto pubSess = makeSession();
  doPublishNamespace(pubSess);

  SubscribeNamespace subNs;
  subNs.trackNamespacePrefix = kNs;
  subNs.requestID = RequestID(nextId_++);
  subNs.options = SubscribeNamespaceOptions::BOTH;
  subNs.forward = false; // subscriber does not want forwarding

  TrackRequestParameter p;
  p.key = folly::to_underlying(TrackRequestParamKey::TRACK_FILTER);
  p.asTrackFilter = TrackFilter{kPropType, 2};
  subNs.params.insertParam(p);

  auto viewer = makeSession();
  withSession(viewer, [&] {
    auto result = folly::coro::blockingWait(
        relay_->subscribeNamespace(std::move(subNs), nullptr), exec_.get());
    EXPECT_TRUE(result.hasValue());
  });

  auto cA = doPublish(pubSess, "a", 100);
  auto cB = doPublish(pubSess, "b", 80);
  auto cC = doPublish(pubSess, "c", 40);
  exec_->driveFor(20);

  // forward=false should not affect track selection: top-2 still receive publish
  EXPECT_EQ(publishCount(viewer.get(), ftn("a")), 1);
  EXPECT_EQ(publishCount(viewer.get(), ftn("b")), 1);
  EXPECT_EQ(publishCount(viewer.get(), ftn("c")), 0); // outside top-2

  cA->publishDone(makePublishDone());
  cB->publishDone(makePublishDone());
  cC->publishDone(makePublishDone());
}

// ---------------------------------------------------------------------------
// Tests: unsubscribe cleans up ranking
// ---------------------------------------------------------------------------

TEST_F(MoqxTrackFilterTest, Unsubscribe_RemovesFromRanking) {
  auto pubSess = makeSession();
  doPublishNamespace(pubSess);

  auto viewer = makeSession();
  auto subHandle = doSubscribeFilter(viewer, /*maxSelected=*/2);
  ASSERT_NE(subHandle, nullptr);

  auto cA = doPublish(pubSess, "a", 100);
  exec_->driveFor(10);

  // Unsubscribe — should not crash and ranking should be cleaned up
  subHandle->unsubscribeNamespace();
  exec_->driveFor(10);

  // Publishing another track should not crash after unsubscribe
  auto cB = doPublish(pubSess, "b", 80);
  exec_->driveFor(10);

  cA->publishDone(makePublishDone());
  cB->publishDone(makePublishDone());
}

// Late subscriber joins, then a previously-quiet track sends a value update
// that pushes it into top-N. Verifies that getOrCreateRanking wires the
// TopNFilter observer correctly so subsequent value changes are tracked.
TEST_F(MoqxTrackFilterTest, PublishFirst_LateSubscriber_ValueChangeUpdatesRanking) {
  auto pubSess = makeSession();
  doPublishNamespace(pubSess);

  // "loud" (100) and "quiet" (20) published before subscriber joins
  auto consumerLoud = doPublish(pubSess, "loud", 100);
  auto consumerQuiet = doPublish(pubSess, "quiet", 20);
  exec_->driveFor(10);

  // Late subscriber: top-1 only
  auto viewer = makeSession();
  auto subHandle = doSubscribeFilter(viewer, /*maxSelected=*/1);
  ASSERT_NE(subHandle, nullptr);
  exec_->driveFor(20);

  // Initial selection: "loud" should be the top-1
  ASSERT_EQ(publishCount(viewer.get(), ftn("loud")), 1);
  ASSERT_EQ(publishCount(viewer.get(), ftn("quiet")), 0);

  // "quiet" jumps above "loud" — observer wired by getOrCreateRanking must fire
  sendValue(consumerQuiet, 200);
  exec_->driveFor(20);

  // "quiet" should now enter top-1
  EXPECT_EQ(publishCount(viewer.get(), ftn("quiet")), 1);

  consumerLoud->publishDone(makePublishDone());
  consumerQuiet->publishDone(makePublishDone());
}

// ---------------------------------------------------------------------------
// Tests: track ending (PUBLISH_DONE) while selected
// ---------------------------------------------------------------------------

// When the top-N track sends PUBLISH_DONE, the ranking should remove it and
// promote the next candidate. This exercises the notifyTrackEnded→removeTrack
// path without the redundant direct-loop in onPublishDone.
TEST_F(MoqxTrackFilterTest, PublishDone_SelectedTrack_PromotesReplacement) {
  auto pubSess = makeSession();
  doPublishNamespace(pubSess);

  auto viewer = makeSession();
  doSubscribeFilter(viewer, /*maxSelected=*/1);

  auto consumerA = doPublish(pubSess, "a", 100);
  auto consumerB = doPublish(pubSess, "b", 50);
  exec_->driveFor(20);

  ASSERT_EQ(publishCount(viewer.get(), ftn("a")), 1);
  ASSERT_EQ(publishCount(viewer.get(), ftn("b")), 0);

  // "a" ends: ranking must clean up and promote "b" into top-1.
  consumerA->publishDone(makePublishDone());
  exec_->driveFor(20);

  EXPECT_EQ(publishCount(viewer.get(), ftn("b")), 1);

  consumerB->publishDone(makePublishDone());
}

// PUBLISH_DONE on a non-selected track should not crash and should not cause
// spurious selection of other tracks.
TEST_F(MoqxTrackFilterTest, PublishDone_NonSelectedTrack_NoSpuriousSelection) {
  auto pubSess = makeSession();
  doPublishNamespace(pubSess);

  auto viewer = makeSession();
  doSubscribeFilter(viewer, /*maxSelected=*/1);

  auto consumerA = doPublish(pubSess, "a", 100);
  auto consumerB = doPublish(pubSess, "b", 50);
  exec_->driveFor(20);

  ASSERT_EQ(publishCount(viewer.get(), ftn("a")), 1);

  // "b" ends while outside top-1; no new selection should occur.
  consumerB->publishDone(makePublishDone());
  exec_->driveFor(20);

  EXPECT_EQ(publishCount(viewer.get(), ftn("a")), 1); // unchanged
  EXPECT_EQ(publishCount(viewer.get(), ftn("b")), 0);

  consumerA->publishDone(makePublishDone());
}

// ---------------------------------------------------------------------------
// Tests: value decrease causes track to drop out of top-N
// ---------------------------------------------------------------------------

// A selected track's value drops below an unselected track's value; the
// unselected one should be promoted and the old top-track should be evicted.
TEST_F(MoqxTrackFilterTest, ValueDecrease_TopTrackDropsOutOfTopN) {
  auto pubSess = makeSession();
  doPublishNamespace(pubSess);

  auto viewer = makeSession();
  doSubscribeFilter(viewer, /*maxSelected=*/1);

  auto consumerA = doPublish(pubSess, "a", 100);
  auto consumerB = doPublish(pubSess, "b", 50);
  exec_->driveFor(20);

  ASSERT_EQ(publishCount(viewer.get(), ftn("a")), 1);
  ASSERT_EQ(publishCount(viewer.get(), ftn("b")), 0);

  // "a" drops below "b": "b" should enter top-1.
  sendValue(consumerA, 20);
  exec_->driveFor(20);

  EXPECT_EQ(publishCount(viewer.get(), ftn("b")), 1);

  consumerA->publishDone(makePublishDone());
  consumerB->publishDone(makePublishDone());
}

// ---------------------------------------------------------------------------
// Tests: direct subscriber and TRACK_FILTER subscriber on the same namespace
// ---------------------------------------------------------------------------

// A direct (non-filtered) subscriber must receive all published tracks while
// the TRACK_FILTER subscriber receives only the top-N subset.
TEST_F(MoqxTrackFilterTest, DirectAndFilterSubscriberCoexist) {
  auto pubSess = makeSession();
  doPublishNamespace(pubSess);

  auto directViewer = makeSession();
  doSubscribeDirect(directViewer);

  auto filteredViewer = makeSession();
  doSubscribeFilter(filteredViewer, /*maxSelected=*/1);

  auto cA = doPublish(pubSess, "a", 100);
  auto cB = doPublish(pubSess, "b", 50);
  auto cC = doPublish(pubSess, "c", 20);
  exec_->driveFor(20);

  // Direct subscriber sees all three tracks.
  EXPECT_EQ(publishCount(directViewer.get(), ftn("a")), 1);
  EXPECT_EQ(publishCount(directViewer.get(), ftn("b")), 1);
  EXPECT_EQ(publishCount(directViewer.get(), ftn("c")), 1);

  // TRACK_FILTER subscriber sees only the top-1.
  EXPECT_EQ(publishCount(filteredViewer.get(), ftn("a")), 1);
  EXPECT_EQ(publishCount(filteredViewer.get(), ftn("b")), 0);
  EXPECT_EQ(publishCount(filteredViewer.get(), ftn("c")), 0);

  cA->publishDone(makePublishDone());
  cB->publishDone(makePublishDone());
  cC->publishDone(makePublishDone());
}

// ---------------------------------------------------------------------------
// Tests: trackFilterSubscribers_ cleanup on unsubscribe
// ---------------------------------------------------------------------------

// After a TRACK_FILTER subscriber unsubscribes, a subsequent subscriber with
// the same N should receive the correct top-N independently. Stale entries
// left over by the first subscriber must not interfere with selection or
// eviction for the second subscriber.
TEST_F(MoqxTrackFilterTest, Unsubscribe_StaleEntriesDoNotAffectResubscribe) {
  auto pubSess = makeSession();
  doPublishNamespace(pubSess);

  auto cA = doPublish(pubSess, "a", 100);
  auto cB = doPublish(pubSess, "b", 50);
  exec_->driveFor(10);

  // First subscriber joins, "a" is selected, then unsubscribes.
  // Without the fix, {a, viewer1*} lingers in trackFilterSubscribers_.
  auto viewer1 = makeSession();
  auto subHandle = doSubscribeFilter(viewer1, /*maxSelected=*/1);
  ASSERT_NE(subHandle, nullptr);
  exec_->driveFor(10);
  ASSERT_EQ(publishCount(viewer1.get(), ftn("a")), 1);

  subHandle->unsubscribeNamespace();
  exec_->driveFor(10);

  // Second subscriber should get the correct top-1 with no interference.
  auto viewer2 = makeSession();
  doSubscribeFilter(viewer2, /*maxSelected=*/1);
  exec_->driveFor(20);

  EXPECT_EQ(publishCount(viewer2.get(), ftn("a")), 1);
  EXPECT_EQ(publishCount(viewer2.get(), ftn("b")), 0);

  // A new higher-value track should be selected for viewer2 only.
  auto cC = doPublish(pubSess, "c", 200);
  exec_->driveFor(20);

  EXPECT_EQ(publishCount(viewer2.get(), ftn("c")), 1);

  // viewer1 was unsubscribed and must not receive anything new.
  EXPECT_EQ(publishCount(viewer1.get(), ftn("b")), 0);
  EXPECT_EQ(publishCount(viewer1.get(), ftn("c")), 0);

  cA->publishDone(makePublishDone());
  cB->publishDone(makePublishDone());
  cC->publishDone(makePublishDone());
}

// ---------------------------------------------------------------------------
// Tests: deselected queue eviction
// ---------------------------------------------------------------------------

// When the deselected queue overflows maxDeselected, the oldest entry is
// evicted (subscriber removed from forwarder). This test uses maxDeselected=2
// to keep the setup minimal: three displaced tracks fills the queue to 3.
TEST_F(MoqxTrackFilterTest, DeselectedQueueEviction_EvictsOldestEntry) {
  // Override relay_ with a tighter maxDeselected so eviction triggers quickly.
  relay_ = std::make_shared<MoqxRelay>(
      config::CacheConfig{0, 0}, // no cache
      /*relayID=*/"",
      /*maxDeselected=*/2);
  relay_->setAllowedNamespacePrefix(kPrefix);

  auto pubSess = makeSession();
  doPublishNamespace(pubSess);

  auto viewer = makeSession();
  doSubscribeFilter(viewer, /*maxSelected=*/1);

  // "base" enters top-1 first.
  auto cBase = doPublish(pubSess, "base", 10);
  exec_->driveFor(10);
  ASSERT_EQ(publishCount(viewer.get(), ftn("base")), 1);

  // Each successive track displaces the previous top-1 into the deselected queue.
  // After "t3" arrives the queue is {"base","t1","t2"} (size 3 > maxDeselected=2),
  // which evicts "base".
  auto cT1 = doPublish(pubSess, "t1", 20); // deselected: {base}
  auto cT2 = doPublish(pubSess, "t2", 30); // deselected: {base, t1}
  auto cT3 = doPublish(pubSess, "t3", 40); // deselected overflows → "base" evicted
  exec_->driveFor(20);

  // All four tracks entered top-1 in sequence as higher-value tracks arrived.
  EXPECT_EQ(publishCount(viewer.get(), ftn("base")), 1);
  EXPECT_EQ(publishCount(viewer.get(), ftn("t1")), 1);
  EXPECT_EQ(publishCount(viewer.get(), ftn("t2")), 1);
  EXPECT_EQ(publishCount(viewer.get(), ftn("t3")), 1);

  // Verify total number of selections: all 4 tracks should have been selected
  // exactly once as they successively entered top-1.
  auto allPublished = published(viewer.get());
  EXPECT_EQ(allPublished.size(), 4);

  cBase->publishDone(makePublishDone());
  cT1->publishDone(makePublishDone());
  cT2->publishDone(makePublishDone());
  cT3->publishDone(makePublishDone());
}

// ---------------------------------------------------------------------------
// Tests: filter chain installation in both publish and subscribe paths
// ---------------------------------------------------------------------------

// Verifies that when a TRACK_FILTER subscriber joins after PUBLISH, the relay
// correctly handles forwarding. The forwardChanged() callback mechanism sends
// REQUEST_UPDATE when forwarding state changes.
TEST_F(MoqxTrackFilterTest, ForwardStateUpdatedWhenFilterSubscriberJoins) {
  auto pubSess = makeSession();
  doPublishNamespace(pubSess);

  // Publish tracks with no subscribers initially (forward would be false/true
  // depending on whether TRACK_FILTER subscribers exist at publish time)
  auto consumerA = doPublish(pubSess, "a", 100);
  auto consumerB = doPublish(pubSess, "b", 50);
  exec_->driveFor(10);

  // TRACK_FILTER subscriber joins - should trigger forwardChanged() callback
  // which sends REQUEST_UPDATE with forward=true to the publisher
  auto viewer = makeSession();
  doSubscribeFilter(viewer, /*maxSelected=*/1);
  exec_->driveFor(20);

  // Verify subscriber receives the top-1 track
  EXPECT_EQ(publishCount(viewer.get(), ftn("a")), 1);
  EXPECT_EQ(publishCount(viewer.get(), ftn("b")), 0);

  // Value changes should still work (verifies forwarder is receiving objects)
  sendValue(consumerB, 200);
  exec_->driveFor(20);

  EXPECT_EQ(publishCount(viewer.get(), ftn("b")), 1);

  consumerA->publishDone(makePublishDone());
  consumerB->publishDone(makePublishDone());
}

// Verifies that TopNFilter is installed in the filter chain for both PUBLISH
// and SUBSCRIBE paths. A direct subscriber joins first (triggering filter chain
// creation), then a TRACK_FILTER subscriber joins later. Value changes on
// existing tracks must be observed and reflected in the TRACK_FILTER ranking.
TEST_F(MoqxTrackFilterTest, FilterChainInstalledForBothPaths) {
  auto pubSess = makeSession();
  doPublishNamespace(pubSess);

  // Direct subscriber joins first — establishes the relay subscription path
  auto directViewer = makeSession();
  doSubscribeDirect(directViewer);

  // Publisher publishes tracks (direct subscriber receives all)
  auto consumerA = doPublish(pubSess, "a", 100);
  auto consumerB = doPublish(pubSess, "b", 50);
  auto consumerC = doPublish(pubSess, "c", 20);
  exec_->driveFor(20);

  ASSERT_EQ(publishCount(directViewer.get(), ftn("a")), 1);
  ASSERT_EQ(publishCount(directViewer.get(), ftn("b")), 1);
  ASSERT_EQ(publishCount(directViewer.get(), ftn("c")), 1);

  // TRACK_FILTER subscriber joins later — must see correct top-1 based on
  // current values at the time of join
  auto filteredViewer = makeSession();
  doSubscribeFilter(filteredViewer, /*maxSelected=*/1);
  exec_->driveFor(20);

  // "a" (100) is top-1
  EXPECT_EQ(publishCount(filteredViewer.get(), ftn("a")), 1);
  EXPECT_EQ(publishCount(filteredViewer.get(), ftn("b")), 0);
  EXPECT_EQ(publishCount(filteredViewer.get(), ftn("c")), 0);

  // Value change: "c" jumps to top — filter chain must observe this change
  // even though tracks were published before TRACK_FILTER subscriber joined
  sendValue(consumerC, 200);
  exec_->driveFor(20);

  // "c" (200) should now be selected for filteredViewer
  EXPECT_EQ(publishCount(filteredViewer.get(), ftn("c")), 1);

  // Direct viewer is unaffected by ranking changes
  EXPECT_EQ(publishCount(directViewer.get(), ftn("a")), 1);
  EXPECT_EQ(publishCount(directViewer.get(), ftn("b")), 1);
  EXPECT_EQ(publishCount(directViewer.get(), ftn("c")), 1);

  consumerA->publishDone(makePublishDone());
  consumerB->publishDone(makePublishDone());
  consumerC->publishDone(makePublishDone());
}

} // namespace
