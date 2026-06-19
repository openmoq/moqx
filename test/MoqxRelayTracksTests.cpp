/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Originally from github.com/facebookexperimental/moxygen.
 * See deps/moxygen/LICENSE for the original license terms.
 *
 * Copyright (c) OpenMOQ contributors.
 */

// Draft 18+: SUBSCRIBE_TRACKS relay tests.
// Ported from deps/moxygen/moxygen/relay/test/MoQRelayTest.cpp

#include "MoqxRelayTestFixture.h"

namespace moxygen::test {

// ============================================================================
// MoqxRelayTracksTest fixture
// ============================================================================

class MoqxRelayTracksTest : public MoQRelayTest {
protected:
  // Like createMockSession() but reports draft 18 for the negotiated version.
  std::shared_ptr<MockMoQSession> createV18Session() {
    auto session = std::make_shared<NiceMock<MockMoQSession>>(exec_);
    ON_CALL(*session, getNegotiatedVersion())
        .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft18)));
    getOrCreateMockState(session);
    return session;
  }

  Publisher::SubscribeTracksResult subscribeTracks(
      std::shared_ptr<MoQSession> session,
      const TrackNamespace& nsPrefix,
      bool forward = true
  ) {
    SubscribeTracks subTracks;
    subTracks.trackNamespacePrefix = nsPrefix;
    subTracks.forward = forward;
    return withSessionContext(session, [&]() {
      auto task = publisherInterface()->subscribeTracks(std::move(subTracks));
      return folly::coro::blockingWait(std::move(task), exec_.get());
    });
  }

  // Helper mirroring doSubscribeNamespace but for SUBSCRIBE_TRACKS.
  // The handle is stashed for cleanup so unsubscribeTracks() fires before
  // the relay is torn down.
  std::shared_ptr<Publisher::SubscribeTracksHandle>
  doSubscribeTracks(std::shared_ptr<MoQSession> session, const TrackNamespace& nsPrefix) {
    auto res = subscribeTracks(session, nsPrefix);
    EXPECT_TRUE(res.hasValue());
    if (!res.hasValue()) {
      return nullptr;
    }
    auto handle = *res;
    cleanupHandles_.push_back(handle);
    return handle;
  }

  void TearDown() override {
    // Drop handle refs before MoQRelayTest tears down the relay.
    for (auto& handle : cleanupHandles_) {
      if (handle) {
        handle->unsubscribeTracks();
      }
    }
    cleanupHandles_.clear();
    MoQRelayTest::TearDown();
  }

  // Stash handles here for automatic cleanup in TearDown.
  // Tests that manually call unsubscribeTracks() should clear this.
  std::vector<std::shared_ptr<Publisher::SubscribeTracksHandle>> cleanupHandles_;
};

// ============================================================================
// Tests
// ============================================================================

// Pre-draft-18 sessions can't issue SUBSCRIBE_TRACKS at all.
TEST_P(MoqxRelayTracksTest, SubscribeTracksRejectsPreV18) {
  // createMockSession() defaults to kVersionDraftCurrent (draft-14).
  auto session = createMockSession();
  auto res = subscribeTracks(session, TrackNamespace{{"test"}});
  ASSERT_FALSE(res.hasValue());
  EXPECT_EQ(res.error().errorCode, SubscribeTracksErrorCode::NOT_SUPPORTED);
  removeSession(session);
}

// SUBSCRIBE_TRACKS and SUBSCRIBE_NAMESPACE live in independent overlap spaces,
// so the same prefix in both trees must be allowed simultaneously.
TEST_P(MoqxRelayTracksTest, SamePrefixInBothTreesAllowed) {
  auto session = createV18Session();
  TrackNamespace prefix{{"test", "live"}};
  doSubscribeNamespace(session, prefix);
  auto tracksHandle = doSubscribeTracks(session, prefix);
  EXPECT_NE(tracksHandle, nullptr);
  removeSession(session);
}

// New tracks published after a SUBSCRIBE_TRACKS are forwarded to the
// subscribing session as PUBLISH messages.
TEST_P(MoqxRelayTracksTest, NewPublishFanoutToTracksSubscriber) {
  auto subscriber = createV18Session();
  auto publisher = createMockSession();
  setupPublishSucceeds(subscriber);

  doSubscribeTracks(subscriber, kTestNamespace);

  // The relay must issue exactly one PUBLISH to subscriber when the
  // matching track appears.
  EXPECT_CALL(*subscriber, publish(_, _)).Times(1);

  doPublish(publisher, kTestTrackName);
  exec_->driveFor(5);

  ASSERT_TRUE(testing::Mock::VerifyAndClearExpectations(subscriber.get()));
  removeSession(subscriber);
  removeSession(publisher);
  // Flush the async forwarder teardown onto the relay exec (MT modes), else
  // the forwarder keeps the subscriber session + consumer alive past exit.
  driveIfMultiThread();
}

// Draft 18 §10.19: a SUBSCRIBE_TRACKS from a session that already has a
// registration at an exact / ancestor / descendant prefix must be rejected
// with PREFIX_OVERLAP. Cross-session overlap is permitted.
TEST_P(MoqxRelayTracksTest, OverlappingSubscribeTracksRejected) {
  auto session = createV18Session();
  const TrackNamespace base{{"a", "b"}};
  const TrackNamespace ancestor{{"a"}};
  const TrackNamespace descendant{{"a", "b", "c"}};

  // Establish the base registration.
  auto baseHandle = doSubscribeTracks(session, base);
  ASSERT_NE(baseHandle, nullptr);

  // Exact duplicate, ancestor, and descendant must all fail.
  for (const auto& [label, prefix] : std::vector<std::pair<std::string, TrackNamespace>>{
           {"exact", base},
           {"ancestor", ancestor},
           {"descendant", descendant},
       }) {
    SCOPED_TRACE(label);
    auto res = subscribeTracks(session, prefix);
    ASSERT_FALSE(res.hasValue());
    EXPECT_EQ(res.error().errorCode, SubscribeTracksErrorCode::PREFIX_OVERLAP);
  }

  // A different session subscribing to the same prefix is fine — the
  // overlap check is per-session.
  auto otherSession = createV18Session();
  auto otherHandle = doSubscribeTracks(otherSession, base);
  EXPECT_NE(otherHandle, nullptr);

  removeSession(session);
  removeSession(otherSession);
}

// A SUBSCRIBE_TRACKS that arrives after a track is already published must
// receive the existing track immediately via PUBLISH (backfill).
TEST_P(MoqxRelayTracksTest, ExistingPublishEchoedToNewTracksSubscriber) {
  auto subscriber = createV18Session();
  auto publisher = createMockSession();
  setupPublishSucceeds(subscriber);

  doPublish(publisher, kTestTrackName);

  EXPECT_CALL(*subscriber, publish(_, _)).Times(1);
  doSubscribeTracks(subscriber, kTestNamespace);
  exec_->driveFor(5);

  ASSERT_TRUE(testing::Mock::VerifyAndClearExpectations(subscriber.get()));
  removeSession(subscriber);
  removeSession(publisher);
  driveIfMultiThread();
}

// The publisher's own tracks must NOT be echoed back when it issues
// SUBSCRIBE_TRACKS on the same namespace prefix it publishes into.
TEST_P(MoqxRelayTracksTest, NoSelfEchoOnSubscribeTracks) {
  auto publisherSubscriber = createV18Session();
  setupPublishSucceeds(publisherSubscriber);

  doPublish(publisherSubscriber, kTestTrackName);

  // Even though the session subscribes to the namespace it published into,
  // publish() must NOT be called on that session.
  EXPECT_CALL(*publisherSubscriber, publish(_, _)).Times(0);
  doSubscribeTracks(publisherSubscriber, kTestNamespace);
  exec_->driveFor(5);

  ASSERT_TRUE(testing::Mock::VerifyAndClearExpectations(publisherSubscriber.get()));
  removeSession(publisherSubscriber);
  driveIfMultiThread();
}

// Verify that unsubscribeTracks() stops future fan-out to that session.
TEST_P(MoqxRelayTracksTest, UnsubscribeTracksStopsFanout) {
  auto subscriber = createV18Session();
  auto publisher = createMockSession();
  setupPublishSucceeds(subscriber);

  auto handle = doSubscribeTracks(subscriber, kTestNamespace);
  ASSERT_NE(handle, nullptr);

  // Unsubscribe before any publish arrives.
  withSessionContext(subscriber, [&]() { handle->unsubscribeTracks(); });
  // Remove from cleanupHandles_ since we already unsubscribed manually.
  cleanupHandles_.clear();

  // No PUBLISH should reach subscriber after unsubscribe.
  EXPECT_CALL(*subscriber, publish(_, _)).Times(0);
  doPublish(publisher, kTestTrackName);
  exec_->driveFor(5);

  ASSERT_TRUE(testing::Mock::VerifyAndClearExpectations(subscriber.get()));
  removeSession(subscriber);
  removeSession(publisher);
  driveIfMultiThread();
}

INSTANTIATE_TEST_SUITE_P(
    AllModes,
    MoqxRelayTracksTest,
    ::testing::Values(RelayMode::SingleThread, RelayMode::MultiThread, RelayMode::LocalForwarderMT),
    [](const ::testing::TestParamInfo<RelayMode>& info) -> std::string {
      switch (info.param) {
      case RelayMode::SingleThread:
        return "SingleThread";
      case RelayMode::MultiThread:
        return "MultiThread";
      case RelayMode::LocalForwarderMT:
        return "LocalForwarderMT";
      }
      return "Unknown";
    }
);

} // namespace moxygen::test
