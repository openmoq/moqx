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
// TODO: Move TestMoQExecutor to a shared test helper (e.g., test/TestUtils.h)
// ---------------------------------------------------------------------------

class TestMoQExecutor : public MoQFollyExecutorImpl, public folly::DrivableExecutor {
public:
  explicit TestMoQExecutor() : MoQFollyExecutorImpl(&evb_) {}

  void add(folly::Func func) override { MoQFollyExecutorImpl::add(std::move(func)); }

  void drive() override {
    if (auto* evb = getBackingEventBase()) {
      evb->loopOnce();
    }
  }

  // TODO: Audit driveFor() call sites to use minimum iterations needed rather
  // than arbitrary values like 10/20. Consider adding a driveUntilIdle() helper.
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
        /*maxDeselected=*/0
    );
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
              .WillByDefault(Invoke([this, raw, ftn = pub.fullTrackName](const auto&, auto, bool) {
                objectsPerTrack_[raw][ftn]++;
                return folly::makeExpected<MoQPublishError>(folly::unit);
              }));
          ON_CALL(*consumer, publishDone(_))
              .WillByDefault(Invoke([this, raw, ftn = pub.fullTrackName](PublishDone) mutable {
                publishDoneCount_[raw][ftn]++;
                return folly::makeExpected<MoQPublishError>(folly::unit);
              }));
          PublishOk ok{
              pub.requestID,
              true,
              128,
              GroupOrder::Default,
              LocationType::LargestObject,
              std::nullopt,
              std::make_optional(uint64_t(0))
          };
          return Subscriber::PublishConsumerAndReplyTask{
              std::static_pointer_cast<TrackConsumer>(consumer),
              folly::coro::makeTask<folly::Expected<PublishOk, PublishError>>(std::move(ok))
          };
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

  // How many times session received publishDone() for ftn
  int publishDoneCount(MockMoQSession* s, const FullTrackName& ftn) const {
    auto it = publishDoneCount_.find(s);
    if (it == publishDoneCount_.end()) {
      return 0;
    }
    auto it2 = it->second.find(ftn);
    return it2 != it->second.end() ? it2->second : 0;
  }

  // How many objects were delivered to the session for ftn
  int objectCount(MockMoQSession* s, const FullTrackName& ftn) const {
    auto it = objectsPerTrack_.find(s);
    if (it == objectsPerTrack_.end()) {
      return 0;
    }
    auto it2 = it->second.find(ftn);
    return it2 != it->second.end() ? it2->second : 0;
  }

  // All FTNs published to this session (in order)
  std::vector<FullTrackName> published(MockMoQSession* s) const {
    auto it = publishedTracks_.find(s);
    return it != publishedTracks_.end() ? it->second : std::vector<FullTrackName>{};
  }

  // ---- Relay helpers -------------------------------------------------------

  template <typename F> auto withSession(std::shared_ptr<MoQSession> session, F&& f) {
    folly::RequestContextScopeGuard guard;
    folly::RequestContext::get()->setContextData(
        sessionToken_,
        std::make_unique<MoQSession::MoQSessionRequestData>(std::move(session))
    );
    return f();
  }

  // Publish a track; returns the TrackConsumer so caller can send objects.
  std::shared_ptr<TrackConsumer> doPublish(
      std::shared_ptr<MoQSession> session,
      const std::string& trackName,
      uint64_t initialLevel = 0
  ) {
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
  std::shared_ptr<Publisher::SubscribeNamespaceHandle>
  doSubscribeFilter(std::shared_ptr<MoQSession> session, uint64_t maxSelected) {
    SubscribeNamespace subNs;
    subNs.trackNamespacePrefix = kNs;
    subNs.requestID = RequestID(nextId_++);
    subNs.options = SubscribeNamespaceOptions::PUBLISH;
    subNs.forward = true;

    TrackRequestParameter p;
    p.key = folly::to_underlying(TrackRequestParamKey::TRACK_FILTER);
    p.asTrackFilter = TrackFilter{kPropType, maxSelected};
    subNs.params.insertParam(p);

    std::shared_ptr<Publisher::SubscribeNamespaceHandle> handle;
    withSession(session, [&] {
      auto result = folly::coro::blockingWait(
          relay_->subscribeNamespace(std::move(subNs), nullptr),
          exec_.get()
      );
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
    auto handle = std::make_shared<NiceMock<MockSubscriptionHandle>>(std::move(ok));
    // Configure the mock to return success for requestUpdate calls
    ON_CALL(*handle, requestUpdateResult())
        .WillByDefault(Return(folly::makeExpected<RequestError>(RequestOk{})));
    return handle;
  }

  FullTrackName ftn(const std::string& name) const { return FullTrackName{kNs, name}; }

  // Subscribe without TRACK_FILTER (plain namespace subscription)
  void doSubscribeDirect(std::shared_ptr<MoQSession> session) {
    SubscribeNamespace subNs;
    subNs.trackNamespacePrefix = kNs;
    subNs.requestID = RequestID(nextId_++);
    subNs.options = SubscribeNamespaceOptions::PUBLISH;
    subNs.forward = true;
    withSession(session, [&] {
      auto result = folly::coro::blockingWait(
          relay_->subscribeNamespace(std::move(subNs), nullptr),
          exec_.get()
      );
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
  // per-session, per-FTN count of objectStream() calls received
  std::map<MoQSession*, std::map<FullTrackName, int>> objectsPerTrack_;
  // per-session, per-FTN count of publishDone() calls received
  std::map<MoQSession*, std::map<FullTrackName, int>> publishDoneCount_;
};

// ---------------------------------------------------------------------------
// Tests: subscriber joins before publishers
// ---------------------------------------------------------------------------

// Subscriber joins first; only top-N publishers should be forwarded.
TEST_F(MoqxTrackFilterTest, SubscribeFirst_Top2Of3_CorrectTracksForwarded) {
  auto pubSess = makeSession();

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

// Initial track property values must drive ranking regardless of publish order.
// Publish a(40), b(100), c(80): value order selects b+c, but arrivalSeq order
// would select a+b — verifying that forwarder extensions are used as initialValue.
TEST_F(MoqxTrackFilterTest, PublishFirst_LateSubscriber_InitialValuesRankCorrectly) {
  auto pubSess = makeSession();

  auto cA = doPublish(pubSess, "a", 40);
  auto cB = doPublish(pubSess, "b", 100);
  auto cC = doPublish(pubSess, "c", 80);
  exec_->driveFor(10);

  auto viewer = makeSession();
  doSubscribeFilter(viewer, /*maxSelected=*/2);
  exec_->driveFor(10);

  EXPECT_EQ(publishCount(viewer.get(), ftn("b")), 1);
  EXPECT_EQ(publishCount(viewer.get(), ftn("c")), 1);
  EXPECT_EQ(publishCount(viewer.get(), ftn("a")), 0);

  cA->publishDone(makePublishDone());
  cB->publishDone(makePublishDone());
  cC->publishDone(makePublishDone());
}

// Retroactive registration must assign arrivalSeq in lastObjectTime (construction
// time) ascending order, not alphabetically. Publish in order c→a→b with equal
// values; alphabetical would select a+b, but publish-time order should select c+a.
TEST_F(MoqxTrackFilterTest, PublishFirst_LateSubscriber_TieBreaksByPublishTime) {
  auto pubSess = makeSession();

  auto cC = doPublish(pubSess, "c");
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  auto cA = doPublish(pubSess, "a");
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  auto cB = doPublish(pubSess, "b");
  exec_->driveFor(10);

  auto viewer = makeSession();
  doSubscribeFilter(viewer, /*maxSelected=*/2);
  exec_->driveFor(10);

  EXPECT_EQ(publishCount(viewer.get(), ftn("c")), 1);
  EXPECT_EQ(publishCount(viewer.get(), ftn("a")), 1);
  EXPECT_EQ(publishCount(viewer.get(), ftn("b")), 0);

  cC->publishDone(makePublishDone());
  cA->publishDone(makePublishDone());
  cB->publishDone(makePublishDone());
}

// ---------------------------------------------------------------------------
// Tests: dynamic value changes
// ---------------------------------------------------------------------------

// A track outside top-N rises above the threshold and displaces the occupant.
TEST_F(MoqxTrackFilterTest, ValueChange_OutsiderEntersTopN) {
  auto pubSess = makeSession();

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

  // viewer should now receive "b" (entered top-1), and "a" evicted with publishDone
  EXPECT_EQ(publishCount(viewer.get(), ftn("b")), 1);
  EXPECT_EQ(publishDoneCount(viewer.get(), ftn("a")), 1);

  consumerA->publishDone(makePublishDone());
  consumerB->publishDone(makePublishDone());
}

// Two subscribers with different N values see independent top-N sets.
TEST_F(MoqxTrackFilterTest, TwoSubscribers_DifferentN_IndependentSets) {
  auto pubSess = makeSession();

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

  SubscribeNamespace subNs;
  subNs.trackNamespacePrefix = kNs;
  subNs.requestID = RequestID(nextId_++);
  subNs.options = SubscribeNamespaceOptions::PUBLISH;
  subNs.forward = false; // subscriber does not want forwarding

  TrackRequestParameter p;
  p.key = folly::to_underlying(TrackRequestParamKey::TRACK_FILTER);
  p.asTrackFilter = TrackFilter{kPropType, 2};
  subNs.params.insertParam(p);

  auto viewer = makeSession();
  withSession(viewer, [&] {
    auto result = folly::coro::blockingWait(
        relay_->subscribeNamespace(std::move(subNs), nullptr),
        exec_.get()
    );
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

  auto viewer = makeSession();
  auto subHandle = doSubscribeFilter(viewer, /*maxSelected=*/2);
  ASSERT_NE(subHandle, nullptr);

  auto cA = doPublish(pubSess, "a", 100);
  exec_->driveFor(10);

  // Unsubscribe removes session from ranking
  subHandle->unsubscribeNamespace();
  exec_->driveFor(10);

  // New track can still be published after subscriber leaves
  auto cB = doPublish(pubSess, "b", 80);
  exec_->driveFor(10);

  // Verify track "a" was received before unsubscribe, "b" was not
  // (no subscriber to receive it after unsubscribe)
  EXPECT_EQ(publishCount(viewer.get(), ftn("a")), 1);
  EXPECT_EQ(publishCount(viewer.get(), ftn("b")), 0);

  cA->publishDone(makePublishDone());
  cB->publishDone(makePublishDone());
}

// Late subscriber joins, then a previously-quiet track sends a value update
// that pushes it into top-N. Verifies that getOrCreateRanking wires the
// TopNFilter observer correctly so subsequent value changes are tracked.
TEST_F(MoqxTrackFilterTest, PublishFirst_LateSubscriber_ValueChangeUpdatesRanking) {
  auto pubSess = makeSession();

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

  // "quiet" should now enter top-1, "loud" evicted with publishDone
  EXPECT_EQ(publishCount(viewer.get(), ftn("quiet")), 1);
  EXPECT_EQ(publishDoneCount(viewer.get(), ftn("loud")), 1);

  consumerLoud->publishDone(makePublishDone());
  consumerQuiet->publishDone(makePublishDone());
}

// When selected tracks have not emitted their first object yet, the relay must
// not idle-evict them before that first cycle completes. This guards the
// boundary case seen in the live load test where rank N was replaced by N+1.
TEST_F(MoqxTrackFilterTest, FirstObjectCycle_DoesNotEvictSelectedTracksBeforeFirstObject) {
  relay_ = std::make_shared<MoqxRelay>(
      config::CacheConfig{0, 0},
      /*relayID=*/"",
      /*maxDeselected=*/0,
      /*idleTimeout=*/std::chrono::seconds(10),
      /*activityThreshold=*/std::chrono::milliseconds(1)
  );
  relay_->setAllowedNamespacePrefix(kPrefix);

  std::vector<std::shared_ptr<MockMoQSession>> publishers;
  std::vector<std::shared_ptr<TrackConsumer>> consumers;
  publishers.reserve(7);
  consumers.reserve(7);

  for (int i = 0; i < 7; ++i) {
    auto publisher = makeSession();
    publishers.push_back(publisher);
    consumers.push_back(doPublish(publisher, folly::to<std::string>("p", i)));
  }
  exec_->driveFor(10);

  auto viewer = makeSession();
  doSubscribeFilter(viewer, /*maxSelected=*/6);
  exec_->driveFor(20);

  for (int i = 0; i < 7; ++i) {
    sendValue(consumers[i], static_cast<uint64_t>(70 - i));
    exec_->driveFor(5);
  }
  exec_->driveFor(30);

  EXPECT_GT(objectCount(viewer.get(), ftn("p5")), 0);
  EXPECT_EQ(objectCount(viewer.get(), ftn("p6")), 0);

  for (auto& consumer : consumers) {
    consumer->publishDone(makePublishDone());
  }
}

// ---------------------------------------------------------------------------
// Tests: track ending (PUBLISH_DONE) while selected
// ---------------------------------------------------------------------------

// When the top-N track sends PUBLISH_DONE, the ranking should remove it and
// promote the next candidate. This exercises the notifyTrackEnded→removeTrack
// path without the redundant direct-loop in onPublishDone.
TEST_F(MoqxTrackFilterTest, PublishDone_SelectedTrack_PromotesReplacement) {
  auto pubSess = makeSession();

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

// PUBLISH_DONE on a non-selected track should not cause spurious selection.
TEST_F(MoqxTrackFilterTest, PublishDone_NonSelectedTrack_NoSpuriousSelection) {
  auto pubSess = makeSession();

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

  auto viewer = makeSession();
  doSubscribeFilter(viewer, /*maxSelected=*/1);

  auto consumerA = doPublish(pubSess, "a", 100);
  auto consumerB = doPublish(pubSess, "b", 50);
  exec_->driveFor(20);

  ASSERT_EQ(publishCount(viewer.get(), ftn("a")), 1);
  ASSERT_EQ(publishCount(viewer.get(), ftn("b")), 0);

  // "a" drops below "b": "b" should enter top-1, "a" evicted with publishDone.
  sendValue(consumerA, 20);
  exec_->driveFor(20);

  EXPECT_EQ(publishCount(viewer.get(), ftn("b")), 1);
  EXPECT_EQ(publishDoneCount(viewer.get(), ftn("a")), 1);

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
      /*maxDeselected=*/2
  );
  relay_->setAllowedNamespacePrefix(kPrefix);

  auto pubSess = makeSession();

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

  // "base" overflowed the deselected queue and was evicted with publishDone
  EXPECT_EQ(publishDoneCount(viewer.get(), ftn("base")), 1);

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

// ---------------------------------------------------------------------------
// Tests: idle eviction
// ---------------------------------------------------------------------------

// A selected track that stops sending objects is evicted once it has been
// silent for longer than idleTimeout. An outsider that keeps sending objects
// triggers the sweep (via the throttled onActivity callback) and is promoted.
TEST_F(MoqxTrackFilterTest, IdleEviction_SilentTrackReplacedByActiveOutsider) {
  relay_ = std::make_shared<MoqxRelay>(
      config::CacheConfig{0, 0}, // no cache
      /*relayID=*/"",
      /*maxDeselected=*/5,
      /*idleTimeout=*/std::chrono::milliseconds(10),
      /*activityThreshold=*/std::chrono::milliseconds(1)
  );
  relay_->setAllowedNamespacePrefix(kPrefix);

  auto pubSess = makeSession();

  auto viewer = makeSession();
  doSubscribeFilter(viewer, /*maxSelected=*/1);

  // "a" (100) enters top-1; "b" (50) is outside.
  auto consumerA = doPublish(pubSess, "a", 100);
  auto consumerB = doPublish(pubSess, "b", 50);
  exec_->driveFor(20);

  ASSERT_EQ(publishCount(viewer.get(), ftn("a")), 1);
  ASSERT_EQ(publishCount(viewer.get(), ftn("b")), 0);

  // Stamp "a"'s lastObjectTime so it is not treated as epoch-idle.
  sendValue(consumerA, 100);
  exec_->driveFor(5);

  // Schedule an object on "b" after idleTimeout has elapsed. loop() blocks
  // until the timer fires, the sendValue call triggers sweepIdle (which
  // schedules the publishToSession coroutine), and all resulting callbacks
  // drain — then loop() returns naturally with no pending work.
  auto* evb = exec_->getBackingEventBase();
  evb->runAfterDelay([&] { sendValue(consumerB, 50); }, 20 /* ms, > idleTimeout=10ms */);
  evb->loop();

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

// ---------------------------------------------------------------------------
// Tests: publisher-subscriber self-exclusion
// ---------------------------------------------------------------------------

// A session that subscribes with TRACK_FILTER before it starts publishing
// must not receive its own track in the top-N selection.
//
// Setup: pub subscribes N=2, then publishes "self" (100).  Two other
// sessions publish "a" (80) and "b" (60).  Non-self top-2 for pub are
// "a" and "b"; pub must not receive its own "self" track.
// Note: "b" is at shared rank 2 (outside shared top-2 of N=2) because
// "self" occupies a slot, so this also exercises the out-of-shared-top-N
// reconcile path for publisher-subscribers.
TEST_F(MoqxTrackFilterTest, PublisherSubscriber_SubscribeBeforePublish_DoesNotReceiveOwnTrack) {
  auto pub = makeSession();
  auto other = makeSession();

  // Subscribe before publishing.
  doSubscribeFilter(pub, /*maxSelected=*/2);

  auto cSelf = doPublish(pub, "self", 100); // pub's own track
  auto cA = doPublish(other, "a", 80);      // other participant
  auto cB = doPublish(other, "b", 60);      // other participant
  exec_->driveFor(20);

  EXPECT_EQ(publishCount(pub.get(), ftn("self")), 0); // own track excluded
  EXPECT_EQ(publishCount(pub.get(), ftn("a")), 1);
  EXPECT_EQ(publishCount(pub.get(), ftn("b")), 1);

  cSelf->publishDone(makePublishDone());
  cA->publishDone(makePublishDone());
  cB->publishDone(makePublishDone());
}

// A session that publishes before subscribing with TRACK_FILTER must still
// have its already-published track excluded from its personal top-N.
//
// Setup: pub publishes "self" (100); another session publishes "a" (80) and
// "b" (60).  pub subscribes N=2 — must see "a" and "b" but not "self".
TEST_F(MoqxTrackFilterTest, PublisherSubscriber_PublishBeforeSubscribe_StillExcluded) {
  auto pub = makeSession();
  auto other = makeSession();

  // Publish before subscribing.
  auto cSelf = doPublish(pub, "self", 100);
  auto cA = doPublish(other, "a", 80);
  auto cB = doPublish(other, "b", 60);
  exec_->driveFor(10);

  // Late TRACK_FILTER subscription — self-track must be excluded.
  doSubscribeFilter(pub, /*maxSelected=*/2);
  exec_->driveFor(20);

  EXPECT_EQ(publishCount(pub.get(), ftn("self")), 0);
  EXPECT_EQ(publishCount(pub.get(), ftn("a")), 1);
  EXPECT_EQ(publishCount(pub.get(), ftn("b")), 1);

  cSelf->publishDone(makePublishDone());
  cA->publishDone(makePublishDone());
  cB->publishDone(makePublishDone());
}

// A viewer receives all top-N tracks including the publisher-subscriber's
// own track; the publisher-subscriber itself never receives its own stream.
//
// Setup: pub publishes "self" (100); another session publishes "a" (60).
// viewer subscribes N=2 — gets both.  pub subscribes N=2 — gets "a" only.
TEST_F(MoqxTrackFilterTest, PublisherSubscriber_ViewerReceivesSelfTrack) {
  auto pub = makeSession();
  auto other = makeSession();
  auto viewer = makeSession();

  auto cSelf = doPublish(pub, "self", 100);
  auto cA = doPublish(other, "a", 60);
  exec_->driveFor(10);

  doSubscribeFilter(viewer, /*maxSelected=*/2);
  doSubscribeFilter(pub, /*maxSelected=*/2);
  exec_->driveFor(20);

  // Viewer sees both tracks (including pub's self-track).
  EXPECT_EQ(publishCount(viewer.get(), ftn("self")), 1);
  EXPECT_EQ(publishCount(viewer.get(), ftn("a")), 1);

  // Publisher-subscriber never receives its own track.
  EXPECT_EQ(publishCount(pub.get(), ftn("self")), 0);
  EXPECT_EQ(publishCount(pub.get(), ftn("a")), 1);

  cSelf->publishDone(makePublishDone());
  cA->publishDone(makePublishDone());
}

} // namespace
