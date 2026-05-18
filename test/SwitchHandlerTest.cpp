/*
 * Copyright (c) Synamedia
 * SPDX-License-Identifier: Apache-2.0
 */

#include "switch/GroupStartObserver.h"
#include "switch/SwitchAlgorithm.h"
#include "switch/SwitchHandler.h"
#include "switch/SwitchTypes.h"
#include "MoqxRelay.h"
#include "MoqxSession.h"
#include <folly/coro/BlockingWait.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/Request.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <moxygen/events/MoQFollyExecutorImpl.h>
#include <moxygen/relay/MoQForwarder.h>
#include <moxygen/test/MockMoQSession.h>
#include <moxygen/test/Mocks.h>
#include <proxygen/lib/http/webtransport/WebTransport.h>

using namespace testing;
using namespace moxygen;
using namespace moxygen::test;
using namespace openmoq::moqx;

// ── SwitchTypes ───────────────────────────────────────────────────────────────

TEST(SwitchTypesTest, ParameterKeyValue) {
  EXPECT_EQ(kSwitchTransitionParamKey, 0xFF01u);
}

TEST(SwitchTypesTest, SwitchTransitionFields) {
  SwitchTransition st{7, 12};
  EXPECT_EQ(st.switchingGroupID, 7u);
  EXPECT_EQ(st.liveEdgeGroupID, 12u);
}

namespace {

// Mirrors publishForSwitch() encoding: two QUIC VARINTs concatenated.
std::string encodeSwitchTransitionValue(uint64_t g, uint64_t l) {
  folly::IOBufQueue valBuf{folly::IOBufQueue::cacheChainLength()};
  folly::io::QueueAppender va(&valBuf, 16);
  (void)quic::encodeQuicInteger(g, [&](auto b) { va.writeBE(b); });
  (void)quic::encodeQuicInteger(l, [&](auto b) { va.writeBE(b); });
  return valBuf.move()->moveToFbString().toStdString();
}

std::pair<uint64_t, uint64_t> decodeSwitchTransitionValue(
    const std::string& s) {
  auto buf = folly::IOBuf::copyBuffer(s.data(), s.size());
  folly::io::Cursor cursor(buf.get());
  auto g = quic::follyutils::decodeQuicInteger(cursor);
  auto l = quic::follyutils::decodeQuicInteger(cursor);
  EXPECT_TRUE(!!g);
  EXPECT_TRUE(!!l);
  return {g->first, l->first};
}

} // namespace

// Two single-byte QUIC VARINTs (< 64): exact byte verification.
TEST(SwitchTypesTest, SwitchTransitionParamEncoding) {
  // 42 = 0x2A, 63 = 0x3F — both fit in one QUIC VARINT byte.
  auto encoded = encodeSwitchTransitionValue(42, 63);
  ASSERT_EQ(encoded.size(), 2u);
  EXPECT_EQ(static_cast<uint8_t>(encoded[0]), 0x2Au);
  EXPECT_EQ(static_cast<uint8_t>(encoded[1]), 0x3Fu);
}

// Values spanning different QUIC VARINT widths survive round-trip.
TEST(SwitchTypesTest, SwitchTransitionParamRoundTrip) {
  const uint64_t g = 1000;     // 2-byte VARINT (64–16383)
  const uint64_t l = 200000;   // 4-byte VARINT (16384–1073741823)
  auto encoded = encodeSwitchTransitionValue(g, l);
  auto [gDecoded, lDecoded] = decodeSwitchTransitionValue(encoded);
  EXPECT_EQ(gDecoded, g);
  EXPECT_EQ(lDecoded, l);
}

// ── GroupStartObserver ────────────────────────────────────────────────────────

TEST(GroupStartObserverTest, CallbackFiresOnBeginSubgroup) {
  std::vector<uint64_t> observed;
  GroupStartObserver obs([&](uint64_t g) { observed.push_back(g); });

  obs.beginSubgroup(5, 0, Priority(0));
  obs.beginSubgroup(6, 0, Priority(0));
  obs.beginSubgroup(6, 1, Priority(0)); // same group, different subgroup

  ASSERT_EQ(observed.size(), 3u);
  EXPECT_EQ(observed[0], 5u);
  EXPECT_EQ(observed[1], 6u);
  EXPECT_EQ(observed[2], 6u);
}

TEST(GroupStartObserverTest, NoopMethodsReturnUnit) {
  GroupStartObserver obs([](uint64_t) {});

  auto sg = obs.beginSubgroup(0, 0, Priority(0));
  ASSERT_TRUE(sg.hasValue());

  EXPECT_TRUE(obs.setTrackAlias(TrackAlias(1)).hasValue());
  EXPECT_TRUE(obs.awaitStreamCredit().hasValue());
  EXPECT_TRUE(
      obs.objectStream(ObjectHeader(0, 0, 0, std::nullopt, ObjectStatus::NORMAL, {}, std::nullopt),
                       nullptr,
                       false)
          .hasValue());
  EXPECT_TRUE(
      obs.datagram(ObjectHeader(0, 0, 0, std::nullopt, ObjectStatus::NORMAL, {}, std::nullopt),
                   nullptr,
                   false)
          .hasValue());
  EXPECT_TRUE(obs.publishDone(PublishDone{}).hasValue());
}

TEST(GroupStartObserverTest, NoopSubgroupMethodsReturnUnit) {
  GroupStartObserver obs([](uint64_t) {});
  auto sgResult = obs.beginSubgroup(3, 0, Priority(0));
  ASSERT_TRUE(sgResult.hasValue());
  auto& sg = *sgResult.value();

  EXPECT_TRUE(sg.object(0, nullptr, {}, false).hasValue());
  EXPECT_TRUE(sg.endOfGroup(0).hasValue());
  EXPECT_TRUE(sg.endOfTrackAndGroup(0).hasValue());
  EXPECT_TRUE(sg.endOfSubgroup().hasValue());
  sg.reset(ResetStreamErrorCode(0)); // must not crash
}

// ── findGswitch algorithm ─────────────────────────────────────────────────────

TEST(FindGswitchTest, MinimumBeyondBothTracks_ReturnsNullopt) {
  folly::F14FastSet<uint64_t> available{0, 1, 2, 3, 4, 5};
  // min(currentLarge=5, targetLarge=5) = 5 < minimumGroupID=10
  EXPECT_EQ(findGswitch(available, 5, 5, 10), std::nullopt);
}

TEST(FindGswitchTest, AllGroupsAvailable_ReturnsMinimum) {
  folly::F14FastSet<uint64_t> available{0, 1, 2, 3, 4, 5};
  // g=0: check gp=0..4 (targetLarge=5), all in available → ok
  EXPECT_EQ(findGswitch(available, 5, 5, 0), std::optional<uint64_t>(0));
}

TEST(FindGswitchTest, MinimumRespected_SkipsEarlyGroups) {
  folly::F14FastSet<uint64_t> available{0, 1, 2, 3, 4, 5};
  // Groups 0..4 all available but minimumGroupID=3 → gswitch=3
  EXPECT_EQ(findGswitch(available, 5, 5, 3), std::optional<uint64_t>(3));
}

TEST(FindGswitchTest, AtLiveEdge_AlwaysSucceeds) {
  folly::F14FastSet<uint64_t> available{}; // no groups at all
  // g=5, targetLarge=5: inner loop is gp in [5,5) → empty → ok=true
  EXPECT_EQ(findGswitch(available, 5, 5, 5), std::optional<uint64_t>(5));
}

TEST(FindGswitchTest, GapInTarget_SkipsToLiveEdge) {
  folly::F14FastSet<uint64_t> available{0, 2}; // gap at group 1
  // currentLarge=5, targetLarge=5, min=0
  // g=0: gp=0(ok),gp=1(missing) → fail
  // g=1: gp=1(missing) → fail
  // g=2: gp=2(ok),gp=3(missing) → fail
  // g=3: gp=3(missing) → fail
  // g=4: gp=4(missing) → fail
  // g=5: inner loop [5,5) empty → ok → return 5
  EXPECT_EQ(findGswitch(available, 5, 5, 0), std::optional<uint64_t>(5));
}

TEST(FindGswitchTest, TargetAheadOfCurrent_GroupsBeyondCurrentSkipped) {
  // currentLarge=3, targetLarge=8, groups 0..3 all available
  // Groups 4..7 are beyond currentLarge — their availability isn't required
  folly::F14FastSet<uint64_t> available{0, 1, 2, 3};
  // g=0: gp=0..7: gp<=3 must be in available (all are), gp>3 skipped → ok
  EXPECT_EQ(findGswitch(available, 3, 8, 0), std::optional<uint64_t>(0));
}

TEST(FindGswitchTest, CurrentAheadOfTarget_CappedAtMin) {
  // currentLarge=10, targetLarge=4 → range is [min=2, min(10,4)=4]
  folly::F14FastSet<uint64_t> available{2, 3}; // groups 2,3 available
  // g=2: gp=2(ok),gp=3(ok) → inner loop ends at targetLarge=4 → ok
  EXPECT_EQ(findGswitch(available, 10, 4, 2), std::optional<uint64_t>(2));
}

TEST(FindGswitchTest, EmptyAvailableSet_OnlyLiveEdgeWorks) {
  folly::F14FastSet<uint64_t> available{};
  // min(currentLarge=3, targetLarge=3) = 3, minimumGroupID=0
  // g=0..2: inner loop finds missing groups → fail
  // g=3: inner [3,3) empty → ok
  EXPECT_EQ(findGswitch(available, 3, 3, 0), std::optional<uint64_t>(3));
}

// ── SwitchHandler::run() tests ────────────────────────────────────────────────

namespace {

// Minimal executor that drives the folly EventBase for coroutine tests.
class TestMoQExecutor : public MoQFollyExecutorImpl, public folly::DrivableExecutor {
 public:
  explicit TestMoQExecutor() : MoQFollyExecutorImpl(&evb_) {}
  void add(folly::Func func) override { MoQFollyExecutorImpl::add(std::move(func)); }
  void drive() override {
    if (auto* evb = getBackingEventBase()) {
      evb->loopOnce();
    }
  }
  void driveFor(int n) {
    for (int i = 0; i < n; ++i) {
      drive();
    }
  }

 private:
  folly::EventBase evb_;
};

// MoqxSession subclass that captures publishForSwitch/writeCatchup calls
// instead of touching the network.
class FakeMoqxSession : public MoqxSession {
 public:
  explicit FakeMoqxSession(std::shared_ptr<MoQExecutor> exec)
      : MoqxSession(
            folly::MaybeManagedPtr<proxygen::WebTransport>(nullptr),
            std::move(exec)) {}

  struct PublishForSwitchArgs {
    uint64_t switchingGroupID;
    uint64_t liveEdgeGroupID;
    std::optional<SwitchPublishResult> returnValue;
  };

  std::optional<SwitchPublishResult> publishForSwitch(
      moxygen::PublishRequest /*pub*/,
      uint64_t switchingGroupID,
      uint64_t liveEdgeGroupID,
      std::shared_ptr<moxygen::Publisher::SubscriptionHandle> /*handle*/) override {
    publishForSwitchCalls.push_back({switchingGroupID, liveEdgeGroupID, publishForSwitchReturn});
    return publishForSwitchReturn;
  }

  void writeCatchup(
      const moxygen::FullTrackName& trackName,
      uint64_t gswitch,
      uint64_t liveEdge,
      moxygen::RequestID requestID,
      moxygen::MoQCache* /*cache*/) override {
    writeCatchupCalls.push_back({trackName, gswitch, liveEdge, requestID});
  }

  // Recorded calls
  std::vector<PublishForSwitchArgs> publishForSwitchCalls;
  struct WriteCatchupArgs {
    FullTrackName trackName;
    uint64_t gswitch;
    uint64_t liveEdge;
    RequestID requestID;
  };
  std::vector<WriteCatchupArgs> writeCatchupCalls;

  // Control what publishForSwitch returns (default: nullopt → aborts before CUT_OLD)
  std::optional<SwitchPublishResult> publishForSwitchReturn{std::nullopt};
};

// Sets up a folly::RequestContext so that relay methods can identify which
// session is the "current" one (same pattern as MoqxRelayTest).
template <typename Func>
auto withSessionContext(std::shared_ptr<MoQSession> session, Func&& func)
    -> decltype(func()) {
  static folly::RequestToken token("moq_session");
  folly::RequestContextScopeGuard guard;
  folly::RequestContext::get()->setContextData(
      token,
      std::make_unique<MoQSession::MoQSessionRequestData>(std::move(session)));
  return func();
}

} // namespace

// ─── Fixture ─────────────────────────────────────────────────────────────────

class SwitchHandlerRunTest : public ::testing::Test {
 protected:
  const TrackNamespace kNs{{"test", "switch"}};
  const FullTrackName kCurrent{kNs, "current"};
  const FullTrackName kTarget{kNs, "target"};

  void SetUp() override {
    exec_ = std::make_shared<TestMoQExecutor>();
    relay_ = std::make_shared<MoqxRelay>();
    session_ = std::make_shared<FakeMoqxSession>(exec_);
  }

  // Publish a track through the relay from a throw-away MockMoQSession.
  // Returns the TrackConsumer handle for delivering objects to the forwarder.
  std::shared_ptr<TrackConsumer> publishTrack(const FullTrackName& ftn) {
    auto pubSession = std::make_shared<NiceMock<MockMoQSession>>(exec_);
    // The relay holds pubSession in its RelaySubscription::upstream via a
    // shared_ptr cycle (relay → forwarder → callback → relay) that outlives
    // the test fixture.  AllowLeak suppresses the spurious "leaked mock" report.
    testing::Mock::AllowLeak(pubSession.get());
    ON_CALL(*pubSession, getNegotiatedVersion())
        .WillByDefault(Return(std::optional<uint64_t>(kVersionDraftCurrent)));

    SubscribeOk ok;
    ok.requestID = RequestID(0);
    ok.trackAlias = TrackAlias(0);
    ok.expires = std::chrono::milliseconds(0);
    ok.groupOrder = GroupOrder::Default;
    auto handle = std::make_shared<NiceMock<MockSubscriptionHandle>>(std::move(ok));

    PublishRequest pub;
    pub.fullTrackName = ftn;

    return withSessionContext(pubSession, [&]() {
      auto res = relay_->publish(std::move(pub), std::move(handle));
      if (!res.hasValue()) {
        return std::shared_ptr<TrackConsumer>(nullptr);
      }
      return res->consumer;
    });
  }

  // Adds session_ as a subscriber to the named forwarder with a real consumer
  // so that drainSubscriber does not crash (trackConsumer is non-null).
  void subscribeSessionToForwarder(
      const FullTrackName& ftn,
      std::shared_ptr<TrackConsumer> consumer) {
    auto forwarder = relay_->getForwarderByName(ftn);
    ASSERT_NE(forwarder, nullptr);

    SubscribeRequest subReq;
    subReq.fullTrackName = ftn;
    subReq.requestID = RequestID(42);
    subReq.locType = LocationType::LargestObject;
    subReq.forward = false;

    forwarder->addSubscriber(session_, subReq, std::move(consumer));
  }

  // Creates a mock consumer whose setTrackAlias/publishDone succeed by default.
  std::shared_ptr<NiceMock<MockTrackConsumer>> makeSafeConsumer() {
    auto consumer = std::make_shared<NiceMock<MockTrackConsumer>>();
    // The forwarder's Subscriber holds this consumer via shared_ptr.  The
    // relay→forwarder→callback→relay cycle keeps the forwarder alive past test
    // fixture teardown, so the consumer outlives the test.  AllowLeak is safe
    // here because NiceMock has no expectations to verify.
    testing::Mock::AllowLeak(consumer.get());
    ON_CALL(*consumer, setTrackAlias(_))
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
    ON_CALL(*consumer, publishDone(_))
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
    return consumer;
  }

  // Build a moxygen::Switch message targeting kTarget from kCurrent.
  moxygen::Switch makeSwitch(
      RequestID currentReqID = RequestID(0),
      uint64_t minimumGroupID = 0) {
    moxygen::Switch sw;
    sw.currentSubscribeRequestID = currentReqID;
    sw.targetTrackName = kTarget;
    sw.minimumSwitchingGroupID = minimumGroupID;
    return sw;
  }

  // Runs SwitchHandler::run() synchronously and pumps the executor.
  void runHandler(moxygen::Switch sw) {
    SwitchHandler handler(session_, std::move(sw), *relay_);
    folly::coro::blockingWait(handler.run(), exec_.get());
  }

  std::shared_ptr<TestMoQExecutor> exec_;
  std::shared_ptr<MoqxRelay> relay_;
  std::shared_ptr<FakeMoqxSession> session_;
};

// ─── Error path: current forwarder not found ──────────────────────────────────

TEST_F(SwitchHandlerRunTest, NoCurrentForwarder_CompletesWithoutCrash) {
  // Empty relay — getForwarder() returns null → sendErrorPublishDone called
  // publishForSwitch returns nullopt (null wt) → run() finishes cleanly
  ASSERT_NO_FATAL_FAILURE(runHandler(makeSwitch(RequestID(99))));
  // sendErrorPublishDone calls publishForSwitch with (gswitch=0, liveEdge=0)
  ASSERT_EQ(session_->publishForSwitchCalls.size(), 1u);
  EXPECT_EQ(session_->publishForSwitchCalls[0].switchingGroupID, 0u);
  EXPECT_EQ(session_->publishForSwitchCalls[0].liveEdgeGroupID, 0u);
}

// ─── Error path: target forwarder not available ───────────────────────────────

TEST_F(SwitchHandlerRunTest, NoTargetForwarder_CompletesWithDoesNotExist) {
  // Publish current track so getForwarder() returns non-null for session_,
  // then add session_ to that forwarder.  Target track not published.
  auto consumer = makeSafeConsumer();
  publishTrack(kCurrent);
  subscribeSessionToForwarder(kCurrent, consumer);

  ASSERT_NO_FATAL_FAILURE(runHandler(makeSwitch(RequestID(0))));
  // sendErrorPublishDone(DOES_NOT_EXIST, ...) → publishForSwitch(0, 0)
  ASSERT_EQ(session_->publishForSwitchCalls.size(), 1u);
  EXPECT_EQ(session_->publishForSwitchCalls[0].switchingGroupID, 0u);
  EXPECT_EQ(session_->publishForSwitchCalls[0].liveEdgeGroupID, 0u);
}

// ─── Success path: gswitch found immediately from pre-populated data ──────────

TEST_F(SwitchHandlerRunTest, GswitchFoundImmediately_PublishForSwitchCalledCorrectly) {
  // Publish both tracks; set their forwarders to largest=group 10
  auto currentConsumer = makeSafeConsumer();
  auto targetConsumer = makeSafeConsumer();

  publishTrack(kCurrent);
  publishTrack(kTarget);

  auto currentForwarder = relay_->getForwarderByName(kCurrent);
  auto targetForwarder = relay_->getForwarderByName(kTarget);
  ASSERT_NE(currentForwarder, nullptr);
  ASSERT_NE(targetForwarder, nullptr);

  currentForwarder->setLargest({10, 0});
  targetForwarder->setLargest({10, 0});
  // SwitchHandler pre-populates availableTarget from targetForwarder->largest(),
  // so setting largest is sufficient; no beginSubgroup calls needed.

  subscribeSessionToForwarder(kCurrent, currentConsumer);

  // minimumGroupID=5 → gswitch=5 (all groups 5..9 available in target, edge=10)
  runHandler(makeSwitch(RequestID(0), /*minimumGroupID=*/5));

  ASSERT_EQ(session_->publishForSwitchCalls.size(), 1u);
  EXPECT_EQ(session_->publishForSwitchCalls[0].switchingGroupID, 5u);
  EXPECT_EQ(session_->publishForSwitchCalls[0].liveEdgeGroupID, 10u);
}

TEST_F(SwitchHandlerRunTest, MinimumGroupIDAtLiveEdge_PublishForSwitchCalledWithLiveEdge) {
  auto consumer = makeSafeConsumer();
  publishTrack(kCurrent);
  publishTrack(kTarget);

  auto currentForwarder = relay_->getForwarderByName(kCurrent);
  auto targetForwarder = relay_->getForwarderByName(kTarget);
  currentForwarder->setLargest({7, 0});
  targetForwarder->setLargest({7, 0});

  subscribeSessionToForwarder(kCurrent, consumer);

  // minimumGroupID=7 = liveEdge → gswitch=7 (inner loop [7,7) is empty → ok)
  runHandler(makeSwitch(RequestID(0), /*minimumGroupID=*/7));

  ASSERT_EQ(session_->publishForSwitchCalls.size(), 1u);
  EXPECT_EQ(session_->publishForSwitchCalls[0].switchingGroupID, 7u);
  EXPECT_EQ(session_->publishForSwitchCalls[0].liveEdgeGroupID, 7u);
}

// ─── writeCatchup called when gswitch < liveEdge ──────────────────────────────

TEST_F(SwitchHandlerRunTest, WriteCatchupCalledWhenBehindLive) {
  auto currentConsumer = makeSafeConsumer();

  publishTrack(kCurrent);
  publishTrack(kTarget);

  auto currentForwarder = relay_->getForwarderByName(kCurrent);
  auto targetForwarder = relay_->getForwarderByName(kTarget);
  currentForwarder->setLargest({20, 0});
  targetForwarder->setLargest({20, 0});

  subscribeSessionToForwarder(kCurrent, currentConsumer);

  // Arrange: publishForSwitch returns a valid consumer so CUT_OLD + writeCatchup run.
  auto mockConsumer = std::make_shared<NiceMock<MockTrackConsumer>>();
  ON_CALL(*mockConsumer, setTrackAlias(_))
      .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
  ON_CALL(*mockConsumer, publishDone(_))
      .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
  ON_CALL(*mockConsumer, beginSubgroup(_, _, _, _))
      .WillByDefault(Return(folly::makeUnexpected(
          MoQPublishError{MoQPublishError::Code::CANCELLED, ""})));
  session_->publishForSwitchReturn = SwitchPublishResult{mockConsumer};

  // minimumGroupID=10 → gswitch=10, liveEdge=20 → catch-up range [10,20)
  runHandler(makeSwitch(RequestID(0), /*minimumGroupID=*/10));

  ASSERT_EQ(session_->publishForSwitchCalls.size(), 1u);
  EXPECT_EQ(session_->publishForSwitchCalls[0].switchingGroupID, 10u);
  EXPECT_EQ(session_->publishForSwitchCalls[0].liveEdgeGroupID, 20u);

  // writeCatchup must be called with the correct gswitch and liveEdge
  ASSERT_EQ(session_->writeCatchupCalls.size(), 1u);
  EXPECT_EQ(session_->writeCatchupCalls[0].trackName, kTarget);
  EXPECT_EQ(session_->writeCatchupCalls[0].gswitch, 10u);
  EXPECT_EQ(session_->writeCatchupCalls[0].liveEdge, 20u);
}
