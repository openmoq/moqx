/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * In-process integration tests for the NAMESPACE_TOO_LARGE fan-out guard on
 * SUBSCRIBE_NAMESPACE / SUBSCRIBE_TRACKS (see design/namespace-too-large-enforcement.md).
 *
 * Uses a dedicated SingleThread-only fixture (relayExec=nullptr) rather than the
 * parameterized MoQRelayTest so that pre-loading >1000 publishes isn't repeated
 * across all 3 relay modes.
 */

#include "MoqxRelayTestFixture.h"

using namespace openmoq::moqx::test;

namespace {

// Mirrors the anonymous-namespace constant of the same name in src/MoqxRelay.cpp
// (not exposed in MoqxRelay.h — it's an internal DoS-guard threshold).
constexpr uint64_t kMaxNamespaceMatches = 1000;

const TrackNamespace kPrefix{{"conf"}};
const TrackNamespace kNs{{"conf", "room1"}};
constexpr uint64_t kPropType = 0x100;

class MoqxNamespaceTooLargeTest : public openmoq::moqx::test::MoQRelayTest {
protected:
  RelayMode relayMode() const override { return RelayMode::SingleThread; }

  void SetUp() override {
    MoQRelayTest::SetUp();
    relay_ = std::make_shared<MoqxRelay>(
        config::CacheConfig{0, 0}, // no cache
        /*relayID=*/"",
        /*relayHopID=*/0,
        /*relayExec=*/nullptr,
        /*useLocalForwarders=*/false,
        /*maxDeselected=*/0
    );
    relay_->setAllowedNamespacePrefix(kPrefix);
  }

  void TearDown() override {
    relay_.reset();
    exec_->driveFor(10);
  }

  // Mock session that records every publish() (forwarded PUBLISH) it receives.
  std::shared_ptr<MockMoQSession> makeSession(uint64_t version) {
    auto session = std::make_shared<NiceMock<MockMoQSession>>(exec_);
    ON_CALL(*session, getNegotiatedVersion()).WillByDefault(Return(std::optional<uint64_t>(version)));
    ON_CALL(*session, negotiatedSetupExtension(_)).WillByDefault(Return(false));

    auto* raw = session.get();
    ON_CALL(*session, publish(_, _))
        .WillByDefault(Invoke([this, raw](PublishRequest pub, auto) -> Subscriber::PublishResult {
          publishedTracks_[raw].push_back(pub.fullTrackName);
          auto consumer = std::make_shared<NiceMock<MockTrackConsumer>>();
          ON_CALL(*consumer, setTrackAlias(_))
              .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
          ON_CALL(*consumer, publishDone(_))
              .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
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

  int publishCount(MockMoQSession* s) const {
    auto it = publishedTracks_.find(s);
    return it != publishedTracks_.end() ? static_cast<int>(it->second.size()) : 0;
  }

  // Publish `count` distinct tracks under kNs directly via relay_->publish().
  // In SingleThread mode (relayExec_==nullptr) this registers the track in
  // namespaceTree_ synchronously, so no driving is needed to make it visible.
  void publishTracks(std::shared_ptr<MoQSession> session, int count, const std::string& prefix = "t") {
    withSessionContext(session, [&] {
      for (int i = 0; i < count; ++i) {
        PublishRequest pub;
        pub.fullTrackName = FullTrackName{kNs, prefix + std::to_string(i)};
        pub.requestID = RequestID(nextId_++);
        pub.groupOrder = GroupOrder::OldestFirst;
        auto result = relay_->publish(std::move(pub), makePublishHandle());
        ASSERT_TRUE(result.hasValue());
      }
    });
  }

  Publisher::SubscribeNamespaceResult doSubscribeNamespace(
      std::shared_ptr<MoQSession> session,
      std::optional<uint64_t> trackFilterMaxSelected = std::nullopt
  ) {
    SubscribeNamespace subNs;
    subNs.trackNamespacePrefix = kNs;
    subNs.requestID = RequestID(nextId_++);
    subNs.options = SubscribeNamespaceOptions::PUBLISH;
    subNs.forward = true;
    if (trackFilterMaxSelected) {
      TrackRequestParameter p;
      p.key = folly::to_underlying(TrackRequestParamKey::TRACK_FILTER);
      p.asTrackFilter = TrackFilter{kPropType, *trackFilterMaxSelected};
      subNs.params.insertParam(p);
    }
    return withSessionContext(session, [&] {
      return folly::coro::blockingWait(
          relay_->subscribeNamespace(std::move(subNs), nullptr),
          exec_.get()
      );
    });
  }

  Publisher::SubscribeTracksResult doSubscribeTracks(std::shared_ptr<MoQSession> session) {
    SubscribeTracks subTracks;
    subTracks.trackNamespacePrefix = kNs;
    subTracks.requestID = RequestID(nextId_++);
    subTracks.forward = true;
    return withSessionContext(session, [&] {
      return folly::coro::blockingWait(relay_->subscribeTracks(std::move(subTracks)), exec_.get());
    });
  }

  uint64_t nextId_{1};
  std::map<MoQSession*, std::vector<FullTrackName>> publishedTracks_;
};

// ---------------------------------------------------------------------------
// SUBSCRIBE_TRACKS
// ---------------------------------------------------------------------------

// At exactly the threshold (not over it), the subscription succeeds and every
// matching track is backfilled — proving collect-phase count == act-phase forwards.
TEST_F(MoqxNamespaceTooLargeTest, SubscribeTracks_AtThreshold_SucceedsAndForwardsAll) {
  auto pubSession = makeSession(kVersionDraft18);
  publishTracks(pubSession, kMaxNamespaceMatches);

  auto subSession = makeSession(kVersionDraft18);
  auto res = doSubscribeTracks(subSession);
  ASSERT_TRUE(res.hasValue());
  exec_->driveFor(50);

  EXPECT_EQ(publishCount(subSession.get()), static_cast<int>(kMaxNamespaceMatches));
}

// One match over the threshold is rejected before any tree mutation.
TEST_F(MoqxNamespaceTooLargeTest, SubscribeTracks_OverThreshold_RejectedNamespaceTooLarge) {
  auto pubSession = makeSession(kVersionDraft18);
  publishTracks(pubSession, kMaxNamespaceMatches + 1);

  auto subSession = makeSession(kVersionDraft18);
  auto res = doSubscribeTracks(subSession);
  ASSERT_FALSE(res.hasValue());
  EXPECT_EQ(res.error().errorCode, SubscribeTracksErrorCode::NAMESPACE_TOO_LARGE);
  exec_->driveFor(20);
  EXPECT_EQ(publishCount(subSession.get()), 0);
}

// If the rejected SUBSCRIBE_TRACKS had partially registered the subscriber in
// tracksTree_, retrying the identical prefix would fail with PREFIX_OVERLAP
// instead of NAMESPACE_TOO_LARGE. Confirms no partial mutation on rejection.
TEST_F(MoqxNamespaceTooLargeTest, SubscribeTracks_RejectionLeavesNoPartialState) {
  auto pubSession = makeSession(kVersionDraft18);
  publishTracks(pubSession, kMaxNamespaceMatches + 1);

  auto subSession = makeSession(kVersionDraft18);
  auto res1 = doSubscribeTracks(subSession);
  ASSERT_FALSE(res1.hasValue());
  EXPECT_EQ(res1.error().errorCode, SubscribeTracksErrorCode::NAMESPACE_TOO_LARGE);

  auto res2 = doSubscribeTracks(subSession);
  ASSERT_FALSE(res2.hasValue());
  EXPECT_EQ(res2.error().errorCode, SubscribeTracksErrorCode::NAMESPACE_TOO_LARGE);
}

// ---------------------------------------------------------------------------
// SUBSCRIBE_NAMESPACE
// ---------------------------------------------------------------------------

TEST_F(MoqxNamespaceTooLargeTest, SubscribeNamespace_OverThreshold_RejectedNamespaceTooLarge) {
  auto pubSession = makeSession(kVersionDraft18);
  publishTracks(pubSession, kMaxNamespaceMatches + 1);

  auto subSession = makeSession(kVersionDraft18);
  auto res = doSubscribeNamespace(subSession);
  ASSERT_FALSE(res.hasValue());
  EXPECT_EQ(res.error().errorCode, SubscribeNamespaceErrorCode::NAMESPACE_TOO_LARGE);
}

// Gated to draft 18+: an older subscriber is not rejected even over threshold.
TEST_F(MoqxNamespaceTooLargeTest, SubscribeNamespace_OverThreshold_AllowedPreDraft18) {
  auto pubSession = makeSession(kVersionDraft18);
  publishTracks(pubSession, kMaxNamespaceMatches + 1);

  auto subSession = makeSession(kVersionDraftCurrent); // pre-18
  auto res = doSubscribeNamespace(subSession);
  EXPECT_TRUE(res.hasValue());
}

// A rejected SUBSCRIBE_NAMESPACE must not have registered the subscriber:
// tracks published afterwards must not be forwarded to it.
TEST_F(MoqxNamespaceTooLargeTest, SubscribeNamespace_RejectionLeavesSessionUnregistered) {
  auto pubSession = makeSession(kVersionDraft18);
  publishTracks(pubSession, kMaxNamespaceMatches + 1);

  auto subSession = makeSession(kVersionDraft18);
  auto res = doSubscribeNamespace(subSession);
  ASSERT_FALSE(res.hasValue());

  publishTracks(pubSession, 1, "late");
  exec_->driveFor(20);
  EXPECT_EQ(publishCount(subSession.get()), 0);
}

// TRACK_FILTER subscribers with maxSelected < kMaxNamespaceMatches are exempt:
// their steady-state fan-out is bounded by maxSelected regardless of subtree size.
TEST_F(MoqxNamespaceTooLargeTest, SubscribeNamespace_TrackFilterSmallMaxSelected_Exempt) {
  auto pubSession = makeSession(kVersionDraft18);
  publishTracks(pubSession, kMaxNamespaceMatches + 1);

  auto subSession = makeSession(kVersionDraft18);
  auto res = doSubscribeNamespace(subSession, /*trackFilterMaxSelected=*/5);
  EXPECT_TRUE(res.hasValue());
}

// maxSelected >= kMaxNamespaceMatches is NOT exempt: still gets NAMESPACE_TOO_LARGE.
TEST_F(MoqxNamespaceTooLargeTest, SubscribeNamespace_TrackFilterMaxSelectedAtThreshold_NotExempt) {
  auto pubSession = makeSession(kVersionDraft18);
  publishTracks(pubSession, kMaxNamespaceMatches + 1);

  auto subSession = makeSession(kVersionDraft18);
  auto res = doSubscribeNamespace(subSession, /*trackFilterMaxSelected=*/kMaxNamespaceMatches);
  ASSERT_FALSE(res.hasValue());
  EXPECT_EQ(res.error().errorCode, SubscribeNamespaceErrorCode::NAMESPACE_TOO_LARGE);
}

} // namespace
