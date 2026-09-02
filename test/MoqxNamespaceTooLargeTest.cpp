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

constexpr uint64_t kMaxNamespaceMatches = MoqxRelay::kMaxNamespaceMatches;

const TrackNamespace kPrefix{{"conf"}};
const TrackNamespace kNs{{"conf", "room1"}};
constexpr uint64_t kPropType = 0x100;

class MoqxNamespaceTooLargeTest : public openmoq::moqx::test::MoQRelayTest {
protected:
  RelayMode relayMode() const override { return RelayMode::SingleThread; }

  void SetUp() override {
    MoQRelayTest::SetUp();
    relay_->setAllowedNamespacePrefix(kPrefix);
  }

  // Subscriber session that accepts every forwarded PUBLISH; tests assert the count with
  // EXPECT_CALL(*session, publish(_, _)).Times(n) before subscribing.
  std::shared_ptr<MockMoQSession> makeSubscriber(uint64_t version = kVersionDraft18) {
    auto session = createMockSession(version);
    setupPublishSucceeds(session);
    return session;
  }

  // Publish `count` distinct tracks under kNs directly via relay_->publish().
  // In SingleThread mode (relayExec_==nullptr) this registers the track in
  // namespaceTree_ synchronously, so no driving is needed to make it visible.
  void
  publishTracks(std::shared_ptr<MoQSession> session, int count, const std::string& prefix = "t") {
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

  // PUBLISH_NAMESPACE `count` distinct child namespaces of kNs from `session`. These, not
  // tracks, are what a draft-18 NAMESPACE subscriber enumerates.
  void publishNamespaces(std::shared_ptr<MoQSession> session, int count) {
    for (int i = 0; i < count; ++i) {
      auto handle = doPublishNamespace(
          session,
          TrackNamespace(std::vector<std::string>{"conf", "room1", "n" + std::to_string(i)})
      );
      ASSERT_NE(handle, nullptr);
    }
  }

  // options defaults to PUBLISH so the track-count tests exercise the track path; the draft-18
  // wire parser would force NAMESPACE.
  Publisher::SubscribeNamespaceResult doSubscribeNamespace(
      std::shared_ptr<MoQSession> session,
      std::optional<uint64_t> trackFilterMaxSelected = std::nullopt,
      SubscribeNamespaceOptions options = SubscribeNamespaceOptions::PUBLISH
  ) {
    SubscribeNamespace subNs;
    subNs.trackNamespacePrefix = kNs;
    subNs.requestID = RequestID(nextId_++);
    subNs.options = options;
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
};

// ---------------------------------------------------------------------------
// SUBSCRIBE_TRACKS
// ---------------------------------------------------------------------------

// At exactly the threshold (not over it), the subscription succeeds and every
// matching track is backfilled — proving collect-phase count == act-phase forwards.
TEST_F(MoqxNamespaceTooLargeTest, SubscribeTracks_AtThreshold_SucceedsAndForwardsAll) {
  auto pubSession = createMockSession(kVersionDraft18);
  publishTracks(pubSession, kMaxNamespaceMatches);

  auto subSession = makeSubscriber();
  EXPECT_CALL(*subSession, publish(_, _)).Times(static_cast<int>(kMaxNamespaceMatches));
  auto res = doSubscribeTracks(subSession);
  ASSERT_TRUE(res.hasValue());
  exec_->driveFor(50);
}

// One match over the threshold is rejected before any PUBLISH is sent or tree mutation.
// If the first attempt had registered the subscriber in tracksTree_, the retry would fail
// with PREFIX_OVERLAP instead of NAMESPACE_TOO_LARGE.
TEST_F(MoqxNamespaceTooLargeTest, SubscribeTracks_OverThreshold_RejectedWithoutSideEffects) {
  auto pubSession = createMockSession(kVersionDraft18);
  publishTracks(pubSession, kMaxNamespaceMatches + 1);

  auto subSession = makeSubscriber();
  EXPECT_CALL(*subSession, publish(_, _)).Times(0);
  auto res1 = doSubscribeTracks(subSession);
  ASSERT_FALSE(res1.hasValue());
  EXPECT_EQ(res1.error().errorCode, SubscribeTracksErrorCode::NAMESPACE_TOO_LARGE);
  exec_->driveFor(20);

  auto res2 = doSubscribeTracks(subSession);
  ASSERT_FALSE(res2.hasValue());
  EXPECT_EQ(res2.error().errorCode, SubscribeTracksErrorCode::NAMESPACE_TOO_LARGE);
}

// ---------------------------------------------------------------------------
// SUBSCRIBE_NAMESPACE
// ---------------------------------------------------------------------------

// Gated to draft 18+: an older subscriber is not rejected even over threshold.
TEST_F(MoqxNamespaceTooLargeTest, SubscribeNamespace_OverThreshold_AllowedPreDraft18) {
  auto pubSession = createMockSession(kVersionDraft18);
  publishTracks(pubSession, kMaxNamespaceMatches + 1);

  auto subSession = makeSubscriber(kVersionDraft16); // pre-18
  auto res = doSubscribeNamespace(subSession);
  EXPECT_TRUE(res.hasValue());
}

// One match over the threshold is rejected, and the rejected subscriber must not be
// registered: tracks published afterwards must not be forwarded to it.
TEST_F(MoqxNamespaceTooLargeTest, SubscribeNamespace_OverThreshold_RejectedAndUnregistered) {
  auto pubSession = createMockSession(kVersionDraft18);
  publishTracks(pubSession, kMaxNamespaceMatches + 1);

  auto subSession = makeSubscriber();
  EXPECT_CALL(*subSession, publish(_, _)).Times(0);
  auto res = doSubscribeNamespace(subSession);
  ASSERT_FALSE(res.hasValue());
  EXPECT_EQ(res.error().errorCode, SubscribeNamespaceErrorCode::NAMESPACE_TOO_LARGE);

  publishTracks(pubSession, 1, "late");
  exec_->driveFor(20);
}

// Published namespaces (PUBLISH_NAMESPACE) are what a real draft-18 (NAMESPACE) subscriber is
// limited by. A PUBLISH-only subscriber is never sent NAMESPACE messages, so they must not count
// against it. (PUBLISH-only on draft 18 can't arrive off the wire; this guards the in-process
// contract.)
TEST_F(MoqxNamespaceTooLargeTest, SubscribeNamespace_OverThresholdPublishedNamespaces) {
  auto pubSession = createMockSession(kVersionDraft18);
  publishNamespaces(pubSession, kMaxNamespaceMatches + 1);

  auto namespaceSub = makeSubscriber();
  auto rejected =
      doSubscribeNamespace(namespaceSub, std::nullopt, SubscribeNamespaceOptions::NAMESPACE);
  ASSERT_FALSE(rejected.hasValue());
  EXPECT_EQ(rejected.error().errorCode, SubscribeNamespaceErrorCode::NAMESPACE_TOO_LARGE);

  auto publishOnlySub = makeSubscriber();
  EXPECT_TRUE(doSubscribeNamespace(publishOnlySub, std::nullopt, SubscribeNamespaceOptions::PUBLISH)
                  .hasValue());
}

// TRACK_FILTER subscribers with maxSelected < kMaxNamespaceMatches are exempt:
// their steady-state fan-out is bounded by maxSelected regardless of subtree size.
TEST_F(MoqxNamespaceTooLargeTest, SubscribeNamespace_TrackFilterSmallMaxSelected_Exempt) {
  auto pubSession = createMockSession(kVersionDraft18);
  publishTracks(pubSession, kMaxNamespaceMatches + 1);

  auto subSession = makeSubscriber();
  auto res = doSubscribeNamespace(subSession, /*trackFilterMaxSelected=*/5);
  EXPECT_TRUE(res.hasValue());
}

// maxSelected >= kMaxNamespaceMatches is NOT exempt: still gets NAMESPACE_TOO_LARGE.
TEST_F(MoqxNamespaceTooLargeTest, SubscribeNamespace_TrackFilterMaxSelectedAtThreshold_NotExempt) {
  auto pubSession = createMockSession(kVersionDraft18);
  publishTracks(pubSession, kMaxNamespaceMatches + 1);

  auto subSession = makeSubscriber();
  auto res = doSubscribeNamespace(subSession, /*trackFilterMaxSelected=*/kMaxNamespaceMatches);
  ASSERT_FALSE(res.hasValue());
  EXPECT_EQ(res.error().errorCode, SubscribeNamespaceErrorCode::NAMESPACE_TOO_LARGE);
}

} // namespace
