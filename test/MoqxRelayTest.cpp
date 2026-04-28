/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Originally from github.com/facebookexperimental/moxygen.
 * See deps/moxygen/LICENSE for the original license terms.
 *
 * Copyright (c) OpenMOQ contributors.
 */

#include "MoqxRelay.h"
#include "TestUtils.h"
#include "UpstreamProvider.h"
#include <folly/coro/BlockingWait.h>
#include <folly/io/async/EventBase.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <moxygen/MoQTrackProperties.h>
#include <moxygen/events/MoQFollyExecutorImpl.h>
#include <moxygen/relay/MoQForwarder.h>
#include <moxygen/test/MockMoQSession.h>
#include <moxygen/test/Mocks.h>

using namespace testing;
using namespace moxygen;
using namespace openmoq::moqx;

namespace moxygen::test {

using openmoq::moqx::test::makeBuf;

const TrackNamespace kTestNamespace{{"test", "namespace"}};
const TrackNamespace kAllowedPrefix{{"test"}};
const FullTrackName kTestTrackName{kTestNamespace, "track1"};

// TestMoQExecutor that can be driven for tests
class TestMoQExecutor : public MoQFollyExecutorImpl, public folly::DrivableExecutor {
public:
  explicit TestMoQExecutor() : MoQFollyExecutorImpl(&evb_) {}
  ~TestMoQExecutor() override = default;

  void add(folly::Func func) override { MoQFollyExecutorImpl::add(std::move(func)); }

  // Implements DrivableExec::drive
  void drive() override {
    // Run the event loop until there is nothing left to do
    // (simulate a "tick" for test event loop)
    if (auto* evb = getBackingEventBase()) {
      evb->loopOnce();
    }
  }

private:
  folly::EventBase evb_;
};

// Test fixture for MoqxRelay tests
// This provides a skeleton for testing MoqxRelay functionality.
// Full integration tests with real sessions will be added when implementing
// multi-publisher support.
class MoQRelayTest : public ::testing::Test {
protected:
  void SetUp() override {
    exec_ = std::make_shared<TestMoQExecutor>();
    relay_ = std::make_shared<MoqxRelay>(config::CacheConfig{.maxCachedTracks = 0});
    relay_->setAllowedNamespacePrefix(kAllowedPrefix);
  }

  void TearDown() override { relay_.reset(); }

  // Helper to create a mock session
  std::shared_ptr<MockMoQSession> createMockSession() {
    auto session = std::make_shared<NiceMock<MockMoQSession>>(exec_);
    ON_CALL(*session, getNegotiatedVersion())
        .WillByDefault(Return(std::optional<uint64_t>(kVersionDraftCurrent)));
    auto state = getOrCreateMockState(session);
    return session;
  }

  // Helper to create a mock subscription handle for publish calls
  std::shared_ptr<Publisher::SubscriptionHandle> createMockSubscriptionHandle() {
    SubscribeOk ok;
    ok.requestID = RequestID(0);
    ok.trackAlias = TrackAlias(0);
    ok.expires = std::chrono::milliseconds(0);
    ok.groupOrder = GroupOrder::Default;
    auto handle = std::make_shared<NiceMock<MockSubscriptionHandle>>(std::move(ok));
    return handle;
  }

  // Helper to remove a session from the relay and clean up mock state
  void removeSession(std::shared_ptr<MoQSession> sess) { cleanupMockSession(std::move(sess)); }

  // Helper to create a mock consumer with default actions
  std::shared_ptr<MockTrackConsumer> createMockConsumer() {
    auto consumer = std::make_shared<NiceMock<MockTrackConsumer>>();
    ON_CALL(*consumer, setTrackAlias(_))
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
    ON_CALL(*consumer, publishDone(_))
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
    return consumer;
  }

  // Helper to subscribe a session to a track
  std::shared_ptr<SubscriptionHandle> subscribeToTrack(
      std::shared_ptr<MoQSession> session,
      const FullTrackName& trackName,
      std::shared_ptr<TrackConsumer> consumer,
      RequestID requestID = RequestID(0),
      bool addToState = true,
      folly::Optional<SubscribeErrorCode> expectedError = folly::none
  ) {
    SubscribeRequest sub;
    sub.fullTrackName = trackName;
    sub.requestID = requestID;
    sub.locType = LocationType::LargestObject;
    std::shared_ptr<SubscriptionHandle> handle{nullptr};
    withSessionContext(session, [&]() {
      auto task = relay_->subscribe(std::move(sub), consumer);
      auto res = folly::coro::blockingWait(std::move(task), exec_.get());
      if (expectedError.has_value()) {
        // Expect an error and, if present, verify the error code matches
        EXPECT_FALSE(res.hasValue());
        // res has an error; compare error code when available
        const auto& err = res.error();
        // If SubscribeError exposes a code accessor or public member, use it.
        // Assuming err.code exists or err.errorCode() accessor:
        // Adjust the line below to match actual error API.
        EXPECT_EQ(err.errorCode, *expectedError);
      } else {
        EXPECT_TRUE(res.hasValue());
        handle = *res;
        if (addToState) {
          getOrCreateMockState(session)->subscribeHandles.push_back(handle);
        }
      }
    });
    return handle;
  }

  // Helper to set up RequestContext for a session (simulates incoming request)
  template <typename Func>
  auto withSessionContext(std::shared_ptr<MoQSession> session, Func&& func) -> decltype(func()) {
    folly::RequestContextScopeGuard guard;
    folly::RequestContext::get()->setContextData(
        sessionRequestToken(),
        std::make_unique<MoQSession::MoQSessionRequestData>(std::move(session))
    );
    return func();
  }

  static const folly::RequestToken& sessionRequestToken() {
    static folly::RequestToken token("moq_session");
    return token;
  }

  // Helper to simulate session cleanup for mock sessions
  // Real MoQRelaySession calls cleanup() which invokes callbacks on stored
  // state. For mock sessions, we need to manually track and clean up.
  struct MockSessionState {
    std::shared_ptr<MoQSession> session;
    std::vector<std::shared_ptr<TrackConsumer>> publishConsumers;
    // Handles for results from publishNamespace, subscribeNamespace,
    // and subscribe
    std::vector<std::shared_ptr<Subscriber::PublishNamespaceHandle>> publishNamespaceHandles;
    std::vector<std::shared_ptr<Publisher::SubscribeNamespaceHandle>> subscribeNamespaceHandles;
    std::vector<std::shared_ptr<Publisher::SubscriptionHandle>> subscribeHandles;

    void cleanup() {
      // Simulate MoQSession::cleanup() for publish tracks
      // This calls publishDone on all tracked consumers, which triggers
      // FilterConsumer callbacks that properly clean up relay state
      for (auto& consumer : publishConsumers) {
        consumer->publishDone(
            {RequestID(0), PublishDoneStatusCode::SESSION_CLOSED, 0, "mock session cleanup"}
        );
      }
      publishConsumers.clear();

      // Clean up publishNamespaceHandles
      for (auto& handle : publishNamespaceHandles) {
        if (handle) {
          handle->publishNamespaceDone();
        }
      }
      publishNamespaceHandles.clear();

      // Clean up subscribeNamespaceHandles
      for (auto& handle : subscribeNamespaceHandles) {
        if (handle) {
          handle->unsubscribeNamespace();
        }
      }
      subscribeNamespaceHandles.clear();

      // Clean up subscribeHandles
      for (auto& handle : subscribeHandles) {
        if (handle) {
          handle->unsubscribe();
        }
      }
      subscribeHandles.clear();
    }
  };

  std::map<MoQSession*, std::shared_ptr<MockSessionState>> mockSessions_;

  std::shared_ptr<MockSessionState> getOrCreateMockState(std::shared_ptr<MoQSession> session) {
    auto it = mockSessions_.find(session.get());
    if (it == mockSessions_.end()) {
      auto state = std::make_shared<MockSessionState>();
      state->session = session;
      mockSessions_[session.get()] = state;
      return state;
    }
    return it->second;
  }

  void cleanupMockSession(std::shared_ptr<MoQSession> session) {
    auto it = mockSessions_.find(session.get());
    if (it != mockSessions_.end()) {
      // Use withSessionContext to ensure session context is set during cleanup
      withSessionContext(it->second->session, [&]() { it->second->cleanup(); });
      mockSessions_.erase(it);
    }
  }

  // Helper to publish a namespace
  // Returns the PublishNamespaceHandle so tests can use it for manual cleanup
  // if needed If addToState is true, the handle is automatically saved for
  // cleanup
  std::shared_ptr<Subscriber::PublishNamespaceHandle> doPublishNamespace(
      std::shared_ptr<MoQSession> session,
      const TrackNamespace& ns,
      bool addToState = true
  ) {
    PublishNamespace ann;
    ann.trackNamespace = ns;
    return withSessionContext(session, [&]() {
      auto task = relay_->publishNamespace(std::move(ann), nullptr);
      auto res = folly::coro::blockingWait(std::move(task), exec_.get());
      EXPECT_TRUE(res.hasValue());
      if (res.hasValue()) {
        if (addToState) {
          getOrCreateMockState(session)->publishNamespaceHandles.push_back(*res);
        }
        return *res;
      }
      return std::shared_ptr<Subscriber::PublishNamespaceHandle>(nullptr);
    });
  }

  // Helper to publish a track
  // Returns the TrackConsumer so tests can use it for manual operations if
  // needed If addToState is true, the consumer is automatically saved for
  // cleanup
  std::shared_ptr<TrackConsumer> doPublish(
      std::shared_ptr<MoQSession> session,
      const FullTrackName& trackName,
      bool addToState = true
  ) {
    PublishRequest pub;
    pub.fullTrackName = trackName;
    return withSessionContext(session, [&]() {
      auto res = relay_->publish(std::move(pub), createMockSubscriptionHandle());
      EXPECT_TRUE(res.hasValue());
      if (res.hasValue()) {
        if (addToState) {
          getOrCreateMockState(session)->publishConsumers.push_back(res->consumer);
        }
        return res->consumer;
      }
      return std::shared_ptr<TrackConsumer>(nullptr);
    });
  }

  // Helper to subscribe to namespace publishes
  // Returns the SubscribeNamespaceHandle so tests can use it for manual cleanup
  // if needed If addToState is true, the handle is automatically saved for
  // cleanup
  std::shared_ptr<Publisher::SubscribeNamespaceHandle> doSubscribeNamespace(
      std::shared_ptr<MoQSession> session,
      const TrackNamespace& nsPrefix,
      bool addToState = true
  ) {
    SubscribeNamespace subNs;
    subNs.trackNamespacePrefix = nsPrefix;
    return withSessionContext(session, [&]() {
      auto task = relay_->subscribeNamespace(std::move(subNs), nullptr);
      auto res = folly::coro::blockingWait(std::move(task), exec_.get());
      EXPECT_TRUE(res.hasValue());
      if (res.hasValue()) {
        if (addToState) {
          getOrCreateMockState(session)->subscribeNamespaceHandles.push_back(*res);
        }
        return *res;
      }
      return std::shared_ptr<Publisher::SubscribeNamespaceHandle>(nullptr);
    });
  }

  // Helper to set up a mock subgroup consumer with default expectations
  std::shared_ptr<MockSubgroupConsumer> createMockSubgroupConsumer() {
    auto sg = std::make_shared<NiceMock<MockSubgroupConsumer>>();
    ON_CALL(*sg, beginObject(_, _, _, _))
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
    ON_CALL(*sg, objectPayload(_, _))
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(ObjectPublishStatus::IN_PROGRESS)
        ));
    ON_CALL(*sg, endOfSubgroup())
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
    ON_CALL(*sg, endOfGroup(_))
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
    ON_CALL(*sg, endOfTrackAndGroup(_))
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
    return sg;
  }

  // Publish a track with a caller-supplied handle so tests can set expectations
  // on requestUpdateCalled.
  std::shared_ptr<TrackConsumer> doPublishWithHandle(
      std::shared_ptr<MoQSession> session,
      const FullTrackName& trackName,
      std::shared_ptr<Publisher::SubscriptionHandle> handle
  ) {
    return withSessionContext(session, [&]() -> std::shared_ptr<TrackConsumer> {
      PublishRequest pub;
      pub.fullTrackName = trackName;
      auto res = relay_->publish(std::move(pub), std::move(handle));
      EXPECT_TRUE(res.hasValue());
      if (!res.hasValue()) {
        return nullptr;
      }
      auto consumer = res->consumer;
      getOrCreateMockState(session)->publishConsumers.push_back(consumer);
      return consumer;
    });
  }

  // Subscribe to a namespace with an explicit forward flag.
  std::shared_ptr<Publisher::SubscribeNamespaceHandle> doSubscribeNamespaceWithForward(
      std::shared_ptr<MoQSession> session,
      const TrackNamespace& nsPrefix,
      bool forward
  ) {
    SubscribeNamespace subNs;
    subNs.trackNamespacePrefix = nsPrefix;
    subNs.forward = forward;
    return withSessionContext(session, [&]() {
      auto task = relay_->subscribeNamespace(std::move(subNs), nullptr);
      auto res = folly::coro::blockingWait(std::move(task), exec_.get());
      EXPECT_TRUE(res.hasValue());
      if (!res.hasValue()) {
        return std::shared_ptr<Publisher::SubscribeNamespaceHandle>(nullptr);
      }
      getOrCreateMockState(session)->subscribeNamespaceHandles.push_back(*res);
      return *res;
    });
  }

  // Set up session->publish() to succeed with a mock consumer and immediate
  // PublishOk. Call this on any subscriber session before it will receive
  // publishToSession() calls, otherwise the mock returns an error and the
  // subscriber is ejected, confusing REQUEST_UPDATE accounting.
  void setupPublishSucceeds(std::shared_ptr<MockMoQSession> session) {
    ON_CALL(*session, publish(_, _))
        .WillByDefault(Invoke([this](PublishRequest pub, auto) -> Subscriber::PublishResult {
          PublishOk ok{
              pub.requestID,
              /*forward=*/pub.forward,
              /*priority=*/128,
              GroupOrder::Default,
              LocationType::LargestObject,
              /*start=*/std::nullopt,
              /*endGroup=*/std::make_optional(uint64_t(0))
          };
          return Subscriber::PublishConsumerAndReplyTask{
              createMockConsumer(),
              folly::coro::makeTask<folly::Expected<PublishOk, PublishError>>(std::move(ok))
          };
        }));
  }

  // Build a NiceMock SubscriptionHandle suitable for publish() calls.
  std::shared_ptr<NiceMock<MockSubscriptionHandle>> makePublishHandle() {
    SubscribeOk ok;
    ok.requestID = RequestID(0);
    ok.trackAlias = TrackAlias(0);
    ok.expires = std::chrono::milliseconds(0);
    ok.groupOrder = GroupOrder::Default;
    auto handle = std::make_shared<NiceMock<MockSubscriptionHandle>>(std::move(ok));
    ON_CALL(*handle, requestUpdateResult())
        .WillByDefault(Return(folly::makeExpected<RequestError>(RequestOk{})));
    return handle;
  }

  std::shared_ptr<TestMoQExecutor> exec_;
  std::shared_ptr<MoqxRelay> relay_;
};

// Test: Basic relay construction
TEST_F(MoQRelayTest, Construction) {
  EXPECT_NE(relay_, nullptr);
}

// Test: Verify allowed namespace prefix is set correctly
TEST_F(MoQRelayTest, AllowedNamespacePrefix) {
  // This just verifies the relay can be constructed with a namespace prefix
  // More detailed testing requires full session setup
  auto relay2 = std::make_shared<MoqxRelay>(config::CacheConfig{
      .maxCachedTracks = moxygen::kDefaultMaxCachedTracks,
      .maxCachedGroupsPerTrack = moxygen::kDefaultMaxCachedGroupsPerTrack,
  });
  relay2->setAllowedNamespacePrefix(kTestNamespace);
  EXPECT_NE(relay2, nullptr);
}

// Test: MockMoQSession can be created
TEST_F(MoQRelayTest, MockSessionCreation) {
  auto mockSession = createMockSession();
  EXPECT_NE(mockSession, nullptr);
  EXPECT_NE(mockSession->getExecutor(), nullptr);
}

// Test: Publish a track through the relay
TEST_F(MoQRelayTest, PublishSuccess) {
  auto publisherSession = createMockSession();

  // Publish the namespace
  doPublishNamespace(publisherSession, kTestNamespace);

  // Publish the track
  doPublish(publisherSession, kTestTrackName);

  // Cleanup: remove the session from relay to avoid mock leak warning
  removeSession(publisherSession);
}

// Test: Tree pruning when leaf node is removed
// Scenario: test/A/B/C and test/A/D exist. Remove C should prune B but keep A
// and D
TEST_F(MoQRelayTest, PruneLeafKeepSiblings) {
  auto publisherABC = createMockSession();
  auto publisherAD = createMockSession();

  // PublishNamespace test/A/B/C (don't add to state because we
  // publishNamespaceDone manually)
  TrackNamespace nsABC{{"test", "A", "B", "C"}};
  auto handleABC = doPublishNamespace(publisherABC, nsABC, /*addToState=*/false);

  // PublishNamespace test/A/D
  TrackNamespace nsAD{{"test", "A", "D"}};
  doPublishNamespace(publisherAD, nsAD);

  // PublishNamespaceDone test/A/B/C - should prune B (and C) but keep A and D
  withSessionContext(publisherABC, [&]() { handleABC->publishNamespaceDone(); });

  // Verify test/A/D still exists using findPublishNamespaceSessions
  auto sessions = relay_->findPublishNamespaceSessions(nsAD);
  EXPECT_EQ(sessions.size(), 1);
  EXPECT_EQ(sessions[0], publisherAD);

  removeSession(publisherABC);
  removeSession(publisherAD);
}

// Test: Tree pruning removes highest empty ancestor
// Scenario: test/A/B/C only. Remove C should prune A (highest empty after test)
TEST_F(MoQRelayTest, PruneHighestEmptyAncestor) {
  auto publisher = createMockSession();

  // PublishNamespace test/A/B/C (don't add to state because we
  // publishNamespaceDone manually)
  TrackNamespace nsABC{{"test", "A", "B", "C"}};
  auto handle = doPublishNamespace(publisher, nsABC, /*addToState=*/false);

  // PublishNamespaceDone test/A/B/C - should prune A (highest empty ancestor)
  withSessionContext(publisher, [&]() { handle->publishNamespaceDone(); });

  // Try to publishNamespace test/A/B/C again with a new session - should create
  // fresh tree
  auto publisher2 = createMockSession();
  doPublishNamespace(publisher2, nsABC);

  removeSession(publisher);
  removeSession(publisher2);
}

// Test: Pruning happens automatically on removeSession
TEST_F(MoQRelayTest, PruneOnRemoveSession) {
  auto publisher = createMockSession();

  // PublishNamespace deep tree test/A/B/C/D
  TrackNamespace nsABCD{{"test", "A", "B", "C", "D"}};
  doPublishNamespace(publisher, nsABCD);

  // Remove session - should prune entire tree test/A/B/C/D
  removeSession(publisher);

  // Verify we can create test/A/B/C/D again (tree was pruned)
  auto publisher2 = createMockSession();
  doPublishNamespace(publisher2, nsABCD);

  removeSession(publisher2);
}

// Test: No pruning when node still has content (multiple publishers)
TEST_F(MoQRelayTest, NoPruneWhenNodeHasContent) {
  auto publisher1 = createMockSession();
  auto publisher2 = createMockSession();

  // Both publishNamespace test/A/B
  TrackNamespace nsAB{{"test", "A", "B"}};
  auto handle1 = doPublishNamespace(publisher1, nsAB, /*addToState=*/false);
  doPublishNamespace(publisher2, nsAB);

  // PublishNamespaceDone from publisher1 - should NOT prune because publisher2
  // still there
  withSessionContext(publisher1, [&]() { handle1->publishNamespaceDone(); });

  // Verify test/A/B still exists by publishing a track through publisher2
  doPublish(publisher2, FullTrackName{nsAB, "track1"});

  removeSession(publisher1);
  removeSession(publisher2);
}

// Test: EXPOSES BUG - onPublishDone should trigger pruning but doesn't
// This test FAILS because onPublishDone removes the publish from the map
// but doesn't call tryPruneChild to clean up empty nodes
TEST_F(MoQRelayTest, PruneOnPublishDoneBug) {
  auto publisher = createMockSession();

  // Create deep tree test/A/B/C with only a publish (no publishNamespace)
  TrackNamespace nsABC{{"test", "A", "B", "C"}};

  // First publishNamespace so we can publish
  doPublishNamespace(publisher, nsABC);

  // Publish a track
  auto consumer = doPublish(publisher, FullTrackName{nsABC, "track1"});

  // Verify publish exists in the tree
  auto state = relay_->findPublishState(FullTrackName{nsABC, "track1"});
  EXPECT_TRUE(state.nodeExists);
  EXPECT_EQ(state.session, publisher);

  // PublishNamespaceDone - node should stay because publish is still active
  withSessionContext(publisher, [&]() {
    getOrCreateMockState(publisher)->publishNamespaceHandles[0]->publishNamespaceDone();
    getOrCreateMockState(publisher)->publishNamespaceHandles.clear();
  });

  // Publish should still be there, node still exists
  state = relay_->findPublishState(FullTrackName{nsABC, "track1"});
  EXPECT_TRUE(state.nodeExists);
  EXPECT_EQ(state.session, publisher);

  // End the publish - onPublishDone gets called
  withSessionContext(publisher, [&]() {
    consumer->publishDone(
        {RequestID(0), PublishDoneStatusCode::SUBSCRIPTION_ENDED, 0, "publisher done"}
    );
  });

  // BUG EXPOSED: The publish is removed from the map but the node still exists
  state = relay_->findPublishState(FullTrackName{nsABC, "track1"});
  EXPECT_EQ(state.session, nullptr); // No session - PASS

  // THIS FAILS: Node should have been pruned but still exists (memory leak)
  EXPECT_FALSE(state.nodeExists
  ) << "BUG: Node test/A/B/C still exists after publish ended and was the "
       "only content. "
       "onPublishDone should have called tryPruneChild to clean up empty "
       "nodes.";

  removeSession(publisher);
}

// Test: Mixed content types - node with publishNamespace + publish
TEST_F(MoQRelayTest, MixedContentPublishNamespaceAndPublish) {
  auto publisher = createMockSession();

  // PublishNamespace test/A/B
  TrackNamespace nsAB{{"test", "A", "B"}};
  doPublishNamespace(publisher, nsAB);

  // Publish a track in test/A/B
  doPublish(publisher, FullTrackName{nsAB, "track1"});

  // PublishNamespaceDone - should NOT prune because publish still exists
  withSessionContext(publisher, [&]() {
    getOrCreateMockState(publisher)->publishNamespaceHandles[0]->publishNamespaceDone();
    getOrCreateMockState(publisher)->publishNamespaceHandles.clear();
  });

  // Verify node still exists by publishing another track
  doPublish(publisher, FullTrackName{nsAB, "track2"});

  removeSession(publisher);
}

// Test: Mixed content types - node with publishNamespace + sessions
// (subscribers)
TEST_F(MoQRelayTest, MixedContentPublishNamespaceAndSessions) {
  auto publisher = createMockSession();
  auto subscriber = createMockSession();

  // PublishNamespace test/A/B
  TrackNamespace nsAB{{"test", "A", "B"}};
  doPublishNamespace(publisher, nsAB);

  // Subscribe to namespace from another session
  doSubscribeNamespace(subscriber, nsAB);

  // PublishNamespaceDone from publisher - should NOT prune because subscriber
  // still there
  std::shared_ptr<Subscriber::PublishNamespaceHandle> handle =
      getOrCreateMockState(publisher)->publishNamespaceHandles[0];
  getOrCreateMockState(publisher)->publishNamespaceHandles.clear();
  withSessionContext(publisher, [&]() { handle->publishNamespaceDone(); });

  // PublishNamespace again from a different publisher - should work (node still
  // exists)
  auto publisher2 = createMockSession();
  doPublishNamespace(publisher2, nsAB);

  removeSession(publisher);
  removeSession(publisher2);
  removeSession(subscriber);
}

// Test: UnsubscribeNamespace triggers pruning
TEST_F(MoQRelayTest, PruneOnUnsubscribeNamespace) {
  auto subscriber = createMockSession();

  // Subscribe to test/A/B/C namespace (creates tree without publishNamespace)
  TrackNamespace nsABC{{"test", "A", "B", "C"}};
  auto handle = doSubscribeNamespace(subscriber, nsABC, /*addToState=*/false);

  // Unsubscribe - should prune the entire test/A/B/C tree
  withSessionContext(subscriber, [&]() { handle->unsubscribeNamespace(); });

  // Verify tree was pruned by subscribing again - should create fresh tree
  doSubscribeNamespace(subscriber, nsABC);

  removeSession(subscriber);
}

// Test: Middle empty nodes in deep tree
// Scenario: test/A (has publishNamespace), test/A/B (empty), test/A/B/C (has
// publish) Remove C should prune B but keep A
TEST_F(MoQRelayTest, PruneMiddleEmptyNode) {
  auto publisherA = createMockSession();
  auto publisherC = createMockSession();

  // PublishNamespace test/A
  TrackNamespace nsA{{"test", "A"}};
  doPublishNamespace(publisherA, nsA);

  // PublishNamespace test/A/B/C (this creates B as empty intermediate node)
  TrackNamespace nsABC{{"test", "A", "B", "C"}};
  auto handleC = doPublishNamespace(publisherC, nsABC, /*addToState=*/false);

  // PublishNamespaceDone test/A/B/C - should prune B and C but keep A
  withSessionContext(publisherC, [&]() { handleC->publishNamespaceDone(); });

  // Verify test/A still exists
  auto sessionsA = relay_->findPublishNamespaceSessions(nsA);
  EXPECT_EQ(sessionsA.size(), 1);
  EXPECT_EQ(sessionsA[0], publisherA);

  // Verify test/A/B/C was pruned - should be able to publishNamespace it again
  auto publisherC2 = createMockSession();
  doPublishNamespace(publisherC2, nsABC);

  removeSession(publisherA);
  removeSession(publisherC);
  removeSession(publisherC2);
}

// Test: Double publishNamespaceDone doesn't crash or corrupt state
TEST_F(MoQRelayTest, DoublePublishNamespaceDone) {
  auto publisher = createMockSession();

  // PublishNamespace test/A/B
  TrackNamespace nsAB{{"test", "A", "B"}};
  auto handle = doPublishNamespace(publisher, nsAB, /*addToState=*/false);

  // PublishNamespaceDone once
  withSessionContext(publisher, [&]() { handle->publishNamespaceDone(); });

  // PublishNamespaceDone again - should not crash (code handles this
  // gracefully)
  withSessionContext(publisher, [&]() { handle->publishNamespaceDone(); });

  // Verify we can still use the relay
  auto publisher2 = createMockSession();
  doPublishNamespace(publisher2, nsAB);

  removeSession(publisher);
  removeSession(publisher2);
}

// Test: Ownership check in publishNamespaceDone prevents non-owner from
// clearing state When a session calls publishNamespaceDone but is not the owner
// of the namespace, the publishNamespaceDone should be ignored and the real
// owner should remain.
TEST_F(MoQRelayTest, StalePublishNamespaceDoneDoesNotAffectNewOwner) {
  auto publisher1 = createMockSession();
  auto publisher2 = createMockSession();

  // Publisher1 publishNamespaces test/A/B
  TrackNamespace nsAB{{"test", "A", "B"}};
  auto handle1 = doPublishNamespace(publisher1, nsAB, /*addToState=*/false);

  // Publisher2 publishNamespaces a child namespace test/A/B/C
  TrackNamespace nsABC{{"test", "A", "B", "C"}};
  doPublishNamespace(publisher2, nsABC);

  // Publisher2 tries to publishNamespaceDone Publisher1's namespace test/A/B
  // using Publisher1's handle but with Publisher2's session context - should be
  // ignored because publisher2 is not the owner of test/A/B
  withSessionContext(publisher2, [&]() { handle1->publishNamespaceDone(); });

  // Verify publisher1 is STILL the owner by checking
  // findPublishNamespaceSessions returns publisher1 for the namespace. If the
  // ownership check didn't work, sourceSession would be null and
  // findPublishNamespaceSessions would return empty.
  auto sessions = relay_->findPublishNamespaceSessions(nsAB);
  EXPECT_EQ(sessions.size(), 1)
      << "Ownership check failed: findPublishNamespaceSessions returned wrong count";
  if (!sessions.empty()) {
    EXPECT_EQ(sessions[0], publisher1) << "Ownership check failed: wrong session returned";
  }

  // Publisher1 should still be able to properly publishNamespaceDone its own
  // namespace
  withSessionContext(publisher1, [&]() { handle1->publishNamespaceDone(); });

  // Clean up - should not crash
  removeSession(publisher1);
  removeSession(publisher2);
}

// Test: Pruning with multiple children at same level
// Scenario: test/A has children B, C, D. Only B has content.
// Remove B should prune B but keep A, C, D structure intact
TEST_F(MoQRelayTest, PruneOneOfMultipleChildren) {
  auto publisherB = createMockSession();
  auto subscriberC = createMockSession();
  auto subscriberD = createMockSession();

  // PublishNamespace test/A/B
  TrackNamespace nsAB{{"test", "A", "B"}};
  auto handleB = doPublishNamespace(publisherB, nsAB, /*addToState=*/false);

  // Subscribe to test/A/C (creates C as empty node with session)
  TrackNamespace nsAC{{"test", "A", "C"}};
  doSubscribeNamespace(subscriberC, nsAC);

  // Subscribe to test/A/D
  TrackNamespace nsAD{{"test", "A", "D"}};
  doSubscribeNamespace(subscriberD, nsAD);

  // PublishNamespaceDone test/A/B - should prune only B
  withSessionContext(publisherB, [&]() { handleB->publishNamespaceDone(); });

  // Verify test/A still exists (has children C and D)
  // Try to publishNamespace at test/A
  auto publisherA = createMockSession();
  TrackNamespace nsA{{"test", "A"}};
  doPublishNamespace(publisherA, nsA);

  removeSession(publisherB);
  removeSession(publisherA);
  removeSession(subscriberC);
  removeSession(subscriberD);
}

// Test: Empty namespace edge case
TEST_F(MoQRelayTest, EmptyNamespacePublishNamespaceDone) {
  auto publisher = createMockSession();

  // Try to publishNamespace empty namespace (edge case)
  TrackNamespace emptyNs{{}};

  // This might fail or succeed depending on implementation
  // Just verify it doesn't crash
  PublishNamespace ann;
  ann.trackNamespace = emptyNs;
  withSessionContext(publisher, [&]() {
    auto task = relay_->publishNamespace(std::move(ann), nullptr);
    auto res = folly::coro::blockingWait(std::move(task), exec_.get());
    // Don't assert on success/failure, just verify no crash
    if (res.hasValue()) {
      getOrCreateMockState(publisher)->publishNamespaceHandles.push_back(res.value());
    }
  });

  removeSession(publisher);
}

// Test: Verify activeChildCount consistency after complex operations
TEST_F(MoQRelayTest, ActiveChildCountConsistency) {
  auto pub1 = createMockSession();
  auto pub2 = createMockSession();
  auto sub1 = createMockSession();

  // Build tree: test/A/B and test/A/C with different content types
  TrackNamespace nsAB{{"test", "A", "B"}};
  doPublishNamespace(pub1, nsAB);

  TrackNamespace nsAC{{"test", "A", "C"}};
  doSubscribeNamespace(sub1, nsAC);

  // At this point, test/A should have activeChildCount_ == 2 (B and C)
  // We can't directly access private members, but we can verify behavior

  // Remove pub1 (which should remove B and decrement A's count)
  removeSession(pub1);

  // A should still exist (C is still active)
  // Verify by announcing at test/A
  TrackNamespace nsA{{"test", "A"}};
  doPublishNamespace(pub2, nsA);

  // Remove sub1 (which should remove C)
  removeSession(sub1);

  // A should still exist because pub2 published at A
  auto sessions = relay_->findPublishNamespaceSessions(nsA);
  EXPECT_EQ(sessions.size(), 1);
  EXPECT_EQ(sessions[0], pub2);

  removeSession(pub2);
}

// Test: Publish then publishNamespaceDone shouldn't prune while publish active
TEST_F(MoQRelayTest, PublishKeepsNodeAliveAfterPublishNamespaceDone) {
  auto publisher = createMockSession();

  // PublishNamespace test/A/B
  TrackNamespace nsAB{{"test", "A", "B"}};
  doPublishNamespace(publisher, nsAB);

  // Publish track
  doPublish(publisher, FullTrackName{nsAB, "track1"});

  // PublishNamespaceDone (but publish is still active)
  std::shared_ptr<Subscriber::PublishNamespaceHandle> handle =
      getOrCreateMockState(publisher)->publishNamespaceHandles[0];
  getOrCreateMockState(publisher)->publishNamespaceHandles.clear();
  withSessionContext(publisher, [&]() { handle->publishNamespaceDone(); });

  // Try to publish another track - should work (node still exists)
  doPublish(publisher, FullTrackName{nsAB, "track2"});

  removeSession(publisher);
}

// Test: SubscribeNamespace only receives publishes while active
// Sequence: subscribeNamespace (sub1), publish (sub1 gets it),
// beginSubgroup, publishDone, subscribeNamespace (sub2), new publish
// (only sub2 gets it)
TEST_F(MoQRelayTest, SubscribeNamespaceDoesntAddDrainingPublish) {
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
                  /*locType=*/LocationType::LargestObject,
                  /*start=*/std::nullopt,
                  /*endGroup=*/std::nullopt
              };
            }()
        });
      });

  EXPECT_CALL(*mockConsumer1, beginSubgroup(_, _, _, _))
      .WillOnce([&](uint64_t, uint64_t, uint8_t, bool) {
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
}

TEST_F(MoQRelayTest, SubscribeNamespaceEmptyPrefixRejectedPreV16) {
  // Default session uses kVersionDraftCurrent (draft-14, which is < 16)
  auto session = createMockSession();

  TrackNamespace emptyNs{{}};
  SubscribeNamespace subNs;
  subNs.trackNamespacePrefix = emptyNs;

  withSessionContext(session, [&]() {
    auto task = relay_->subscribeNamespace(std::move(subNs), nullptr);
    auto res = folly::coro::blockingWait(std::move(task), exec_.get());
    ASSERT_FALSE(res.hasValue()
    ) << "Empty namespace prefix should be rejected for pre-v16 sessions";
    EXPECT_EQ(res.error().errorCode, SubscribeNamespaceErrorCode::NAMESPACE_PREFIX_UNKNOWN);
    EXPECT_EQ(res.error().reasonPhrase, "empty");
  });

  removeSession(session);
}

TEST_F(MoQRelayTest, SubscribeNamespaceEmptyPrefixAllowedV16) {
  auto session = createMockSession();
  // Override the negotiated version to draft-16
  ON_CALL(*session, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft16)));

  TrackNamespace emptyNs{{}};
  doSubscribeNamespace(session, emptyNs);

  removeSession(session);
}

// ============================================================
// Extensions Tests
// ============================================================

// Test: Extensions from publish are forwarded to subscribers via
// subscribeNamespace
TEST_F(MoQRelayTest, PublishExtensionsForwardedToSubscribers) {
  auto publisherSession = createMockSession();
  auto subscriber = createMockSession();

  // Subscribe to namespace first
  auto mockConsumer = createMockConsumer();
  Extensions receivedExtensions;
  EXPECT_CALL(*subscriber, publish(testing::_, testing::_))
      .WillOnce([&mockConsumer,
                 &receivedExtensions](const PublishRequest& pubReq, auto /*subHandle*/) {
        receivedExtensions = pubReq.extensions;
        return Subscriber::PublishResult(Subscriber::PublishConsumerAndReplyTask{
            mockConsumer,
            []() -> folly::coro::Task<folly::Expected<PublishOk, PublishError>> {
              co_return PublishOk{
                  RequestID(1),
                  true,
                  0,
                  GroupOrder::OldestFirst,
                  LocationType::LargestObject,
                  std::nullopt,
                  std::nullopt
              };
            }()
        });
      });

  doSubscribeNamespace(subscriber, kTestNamespace);

  // Publish with extensions (both known and unknown)
  PublishRequest pub;
  pub.fullTrackName = kTestTrackName;
  pub.extensions.insertMutableExtension(Extension{kDeliveryTimeoutExtensionType, 5000});
  pub.extensions.insertMutableExtension(Extension{0xBEEF'0000, 42});

  withSessionContext(publisherSession, [&]() {
    auto res = relay_->publish(std::move(pub), createMockSubscriptionHandle());
    EXPECT_TRUE(res.hasValue());
    if (res.hasValue()) {
      getOrCreateMockState(publisherSession)->publishConsumers.push_back(res->consumer);
    }
  });
  exec_->drive();

  // Verify extensions were forwarded
  EXPECT_EQ(receivedExtensions.getIntExtension(kDeliveryTimeoutExtensionType), 5000);
  EXPECT_EQ(receivedExtensions.getIntExtension(0xBEEF'0000), 42);

  removeSession(publisherSession);
  removeSession(subscriber);
}

// ============================================================
// Dynamic Groups Extension Tests
// ============================================================

// Test: relay PUBLISH path – dynamic groups from PublishRequest extensions
// is stored in the forwarder and forwarded to every downstream subscriber
TEST_F(MoQRelayTest, RelayPublishPropagatesDynamicGroupsToSubscribers) {
  auto publisherSession = createMockSession();
  auto subscriberSession = createMockSession();

  // Build a PublishRequest with DYNAMIC_GROUPS enabled
  PublishRequest pub;
  pub.fullTrackName = kTestTrackName;
  setPublisherDynamicGroups(pub, true);

  withSessionContext(publisherSession, [&]() {
    auto res = relay_->publish(std::move(pub), createMockSubscriptionHandle());
    ASSERT_TRUE(res.hasValue());
    getOrCreateMockState(publisherSession)->publishConsumers.push_back(res->consumer);
  });

  auto consumer = createMockConsumer();
  auto handle = subscribeToTrack(subscriberSession, kTestTrackName, consumer, RequestID(1));
  ASSERT_NE(handle, nullptr);

  auto dynGroups = getPublisherDynamicGroups(handle->subscribeOk());
  ASSERT_TRUE(dynGroups.has_value());
  EXPECT_TRUE(*dynGroups);

  removeSession(subscriberSession);
  exec_->drive();
  removeSession(publisherSession);
}

// Test: Extensions from publish are forwarded to late-joining subscribers
TEST_F(MoQRelayTest, PublishExtensionsForwardedToLateJoiners) {
  auto publisherSession = createMockSession();
  auto subscriber1 = createMockSession();
  auto subscriber2 = createMockSession();

  // Subscriber 1 subscribes first
  auto mockConsumer1 = createMockConsumer();
  EXPECT_CALL(*subscriber1, publish(testing::_, testing::_))
      .WillOnce([&mockConsumer1](const auto&, auto) {
        return Subscriber::PublishResult(Subscriber::PublishConsumerAndReplyTask{
            mockConsumer1,
            []() -> folly::coro::Task<folly::Expected<PublishOk, PublishError>> {
              co_return PublishOk{
                  RequestID(1),
                  true,
                  0,
                  GroupOrder::OldestFirst,
                  LocationType::LargestObject,
                  std::nullopt,
                  std::nullopt
              };
            }()
        });
      });

  doSubscribeNamespace(subscriber1, kTestNamespace);

  // Publish with extensions
  PublishRequest pub;
  pub.fullTrackName = kTestTrackName;
  pub.extensions.insertMutableExtension(Extension{kDeliveryTimeoutExtensionType, 3000});
  pub.extensions.insertMutableExtension(Extension{0xCAFE'0000, 99});

  withSessionContext(publisherSession, [&]() {
    auto res = relay_->publish(std::move(pub), createMockSubscriptionHandle());
    EXPECT_TRUE(res.hasValue());
    if (res.hasValue()) {
      getOrCreateMockState(publisherSession)->publishConsumers.push_back(res->consumer);
    }
  });
  exec_->drive();

  // Late-joining subscriber 2 should also get extensions
  Extensions receivedExtensions;
  auto mockConsumer2 = createMockConsumer();
  EXPECT_CALL(*subscriber2, publish(testing::_, testing::_))
      .WillOnce([&mockConsumer2, &receivedExtensions](const PublishRequest& pubReq, auto) {
        receivedExtensions = pubReq.extensions;
        return Subscriber::PublishResult(Subscriber::PublishConsumerAndReplyTask{
            mockConsumer2,
            []() -> folly::coro::Task<folly::Expected<PublishOk, PublishError>> {
              co_return PublishOk{
                  RequestID(2),
                  true,
                  0,
                  GroupOrder::OldestFirst,
                  LocationType::LargestObject,
                  std::nullopt,
                  std::nullopt
              };
            }()
        });
      });

  doSubscribeNamespace(subscriber2, kTestNamespace);
  exec_->drive();

  // Verify late-joiner received extensions
  EXPECT_EQ(receivedExtensions.getIntExtension(kDeliveryTimeoutExtensionType), 3000);
  EXPECT_EQ(receivedExtensions.getIntExtension(0xCAFE'0000), 99);

  removeSession(publisherSession);
  removeSession(subscriber1);
  removeSession(subscriber2);
}

// Test: relay SUBSCRIBE path – dynamic groups from the upstream SubscribeOk is
// stored in the forwarder and forwarded to both the first and late-joining
// downstream subscribers
TEST_F(MoQRelayTest, RelaySubscribePropagatesDynamicGroupsToAllSubscribers) {
  auto publisherSession = createMockSession();
  auto subscriber1 = createMockSession();
  auto subscriber2 = createMockSession();

  doPublishNamespace(publisherSession, kTestNamespace);

  // Upstream returns a SubscribeOk with DYNAMIC_GROUPS = true
  SubscribeOk upstreamOk;
  upstreamOk.requestID = RequestID(1);
  upstreamOk.trackAlias = TrackAlias(1);
  upstreamOk.expires = std::chrono::milliseconds(0);
  upstreamOk.groupOrder = GroupOrder::OldestFirst;
  setPublisherDynamicGroups(upstreamOk, true);

  EXPECT_CALL(*publisherSession, subscribe(_, _))
      .WillOnce([upstreamOk](const auto& /*req*/, auto /*consumer*/) {
        auto handle = std::make_shared<NiceMock<MockSubscriptionHandle>>(upstreamOk);
        return folly::coro::makeTask<Publisher::SubscribeResult>(
            folly::Expected<std::shared_ptr<SubscriptionHandle>, SubscribeError>(handle)
        );
      });

  // First subscriber
  auto consumer1 = createMockConsumer();
  auto handle1 = subscribeToTrack(subscriber1, kTestTrackName, consumer1, RequestID(1));
  ASSERT_NE(handle1, nullptr);
  auto dynGroups1 = getPublisherDynamicGroups(handle1->subscribeOk());
  ASSERT_TRUE(dynGroups1.has_value());
  EXPECT_TRUE(*dynGroups1);

  // Late-joining second subscriber – forwarder should propagate the stored
  // dynamic groups value without another upstream roundtrip
  auto consumer2 = createMockConsumer();
  auto handle2 = subscribeToTrack(subscriber2, kTestTrackName, consumer2, RequestID(2));
  ASSERT_NE(handle2, nullptr);
  auto dynGroups2 = getPublisherDynamicGroups(handle2->subscribeOk());
  ASSERT_TRUE(dynGroups2.has_value());
  EXPECT_TRUE(*dynGroups2);

  removeSession(publisherSession);
  removeSession(subscriber1);
  removeSession(subscriber2);
}

TEST_F(MoQRelayTest, ExactNamespaceSubscriberReceivesPublishNamespace) {
  auto subscriber = createMockSession();
  auto publisher = createMockSession();

  // Subscriber subscribes to exact namespace {"test", "namespace"}
  doSubscribeNamespace(subscriber, kTestNamespace);

  // Expect the subscriber to receive a publishNamespace forwarding when
  // the publisher announces the same exact namespace
  EXPECT_CALL(*subscriber, publishNamespace(_, _))
      .WillOnce(
          [](PublishNamespace ann, auto) -> folly::coro::Task<Subscriber::PublishNamespaceResult> {
            EXPECT_EQ(ann.trackNamespace, kTestNamespace);
            co_return folly::makeUnexpected(PublishNamespaceError{
                ann.requestID,
                PublishNamespaceErrorCode::UNINTERESTED,
                "test"
            });
          }
      );

  // Publisher announces the same exact namespace
  doPublishNamespace(publisher, kTestNamespace);

  // Drive the executor so the async publishNamespace forwarding runs
  exec_->drive();

  removeSession(publisher);
  removeSession(subscriber);
}

// Test: TrackStatus on non-existent track
TEST_F(MoQRelayTest, TrackStatusNonExistentTrack) {
  auto clientSession = createMockSession();

  // Request trackStatus for a track that doesn't exist
  TrackStatus trackStatus;
  trackStatus.fullTrackName = kTestTrackName;
  trackStatus.requestID = RequestID(1);

  withSessionContext(clientSession, [&]() {
    auto task = relay_->trackStatus(trackStatus);
    auto res = folly::coro::blockingWait(std::move(task), exec_.get());

    // Should return error indicating track not found
    EXPECT_FALSE(res.hasValue());
    EXPECT_EQ(res.error().errorCode, TrackStatusErrorCode::TRACK_NOT_EXIST);
    EXPECT_FALSE(res.error().reasonPhrase.empty());
  });

  removeSession(clientSession);
}

// Test: TrackStatus on existing track - returns forwarder state (no upstream
// call)
TEST_F(MoQRelayTest, TrackStatusSuccessfulForward) {
  auto publisherSession = createMockSession();
  auto clientSession = createMockSession();

  doPublish(publisherSession, kTestTrackName);

  auto consumer = createMockConsumer();
  subscribeToTrack(clientSession, kTestTrackName, consumer, RequestID(1));

  TrackStatus trackStatus;
  trackStatus.fullTrackName = kTestTrackName;
  trackStatus.requestID = RequestID(2);

  withSessionContext(clientSession, [&]() {
    auto task = relay_->trackStatus(trackStatus);
    auto res = folly::coro::blockingWait(std::move(task), exec_.get());

    // Should return status from local forwarder
    // Since no data was sent, statusCode should be TRACK_NOT_STARTED
    EXPECT_TRUE(res.hasValue());
    EXPECT_EQ(res.value().statusCode, TrackStatusCode::TRACK_NOT_STARTED);
    EXPECT_EQ(res.value().fullTrackName, kTestTrackName);
  });

  removeSession(clientSession);
  exec_->drive();
  removeSession(publisherSession);
}

// Test: TrackStatus using namespace prefix matching (no exact subscription)
// Verifies that when there's no exact subscription but a publisher has
// published a matching namespace prefix, the relay correctly routes
// TRACK_STATUS upstream using prefix matching
TEST_F(MoQRelayTest, TrackStatusViaPrefixMatching) {
  auto publisher = createMockSession();
  auto requester = createMockSession();

  // Publisher publishes namespace but NOT the specific track
  doPublishNamespace(publisher, kTestNamespace);

  // No exact subscription exists for kTestTrackName, so trackStatus should
  // use prefix matching to find the publisher

  // Mock the upstream trackStatus call
  TrackStatusOk statusOk;
  statusOk.requestID = RequestID(1);
  statusOk.trackAlias = TrackAlias(0);
  statusOk.largest = AbsoluteLocation{50, 25};

  EXPECT_CALL(*publisher, trackStatus(_)).WillOnce([statusOk](const auto& /*ts*/) {
    return folly::coro::makeTask<Publisher::TrackStatusResult>(statusOk);
  });

  // Execute trackStatus from requester's perspective
  TrackStatus trackStatus;
  trackStatus.requestID = RequestID(1);
  trackStatus.fullTrackName = kTestTrackName;

  withSessionContext(requester, [&]() {
    auto task = relay_->trackStatus(trackStatus);
    auto result = folly::coro::blockingWait(std::move(task), exec_.get());

    // Should successfully forward via prefix matching and return the result
    EXPECT_TRUE(result.hasValue()) << "TrackStatus via namespace prefix matching should succeed";
    EXPECT_EQ(result.value().requestID, RequestID(1));
    EXPECT_TRUE(result.value().largest.has_value());
    EXPECT_EQ(result.value().largest->group, 50);
    EXPECT_EQ(result.value().largest->object, 25);
  });

  removeSession(publisher);
  removeSession(requester);
}

// Test: makeNamespaceBridgeHandle routes namespaceMsg to doPublishNamespace
TEST_F(MoQRelayTest, NamespaceBridgeHandleForwardsNamespaceMsg) {
  auto peerSession = createMockSession();

  // Bridge handle routes NAMESPACE messages from peerSession into the relay.
  // For peering the subscription prefix is empty, so the suffix == full namespace.
  auto handle = makeNamespaceBridgeHandle(relay_, peerSession);
  handle->namespaceMsg(kTestNamespace);

  auto sessions = relay_->findPublishNamespaceSessions(kTestNamespace);
  ASSERT_EQ(sessions.size(), 1u);
  EXPECT_EQ(sessions[0], peerSession);

  removeSession(peerSession);
}

// Test: makeNamespaceBridgeHandle routes namespaceDoneMsg to doPublishNamespaceDone
TEST_F(MoQRelayTest, NamespaceBridgeHandleForwardsDoneMsg) {
  auto peerSession = createMockSession();
  auto handle = makeNamespaceBridgeHandle(relay_, peerSession);

  handle->namespaceMsg(kTestNamespace);
  ASSERT_EQ(relay_->findPublishNamespaceSessions(kTestNamespace).size(), 1u);

  handle->namespaceDoneMsg(kTestNamespace);
  EXPECT_TRUE(relay_->findPublishNamespaceSessions(kTestNamespace).empty());

  removeSession(peerSession);
}

// Regression test: publisher reconnect after disconnect with active subscriber
// crashes with SIGSEGV at MoqxRelay.cpp:463.
//
// Scenario (relay chain with downstream subscriber that has an open subgroup):
//   1. Publisher session A publishes a track and opens a subgroup.
//   2. Subscriber session B subscribes; the open subgroup is forwarded to B,
//      so B has a live subgroup entry in the forwarder.
//   3. Session A's connection breaks: publishDone fires, onPublishDone() resets
//      handle to null.  The forwarder does NOT remove B because B still has an
//      open subgroup (drainSubscriber marks B as receivedPublishDone_ instead).
//      The subscriptions_ entry therefore survives with handle == nullptr.
//   4. Session A reconnects and re-publishes the same track.  The multipublisher
//      check finds the surviving entry and calls it->second.handle->unsubscribe()
//      — null-pointer dereference, SIGSEGV.
TEST_F(MoQRelayTest, PublisherReconnectWithOpenSubgroupNoSegfault) {
  auto publisherSession = createMockSession();
  auto subscriberSession = createMockSession();

  doPublishNamespace(publisherSession, kTestNamespace);

  // Step 1: publisher session A publishes the track.
  // Don't add consumer to state — we control cleanup manually.
  auto consumer = doPublish(publisherSession, kTestTrackName, /*addToState=*/false);
  ASSERT_NE(consumer, nullptr);

  // Step 2: subscriber session B subscribes.  Wire its mock consumer to return
  // a live SubgroupConsumer so the forwarder stores an open subgroup for B.
  auto mockSubgroupConsumer = createMockSubgroupConsumer();
  auto mockConsumer = createMockConsumer();
  ON_CALL(*mockConsumer, beginSubgroup(_, _, _, _))
      .WillByDefault(Return(folly::makeExpected<MoQPublishError>(
          std::static_pointer_cast<SubgroupConsumer>(mockSubgroupConsumer)
      )));
  auto subHandle = subscribeToTrack(subscriberSession, kTestTrackName, mockConsumer);
  ASSERT_NE(subHandle, nullptr);

  // Open a subgroup through the publisher consumer so that B gets a live entry
  // in its subgroups map inside the forwarder.
  withSessionContext(publisherSession, [&]() {
    auto sgRes = consumer->beginSubgroup(/*groupID=*/0, /*subgroupID=*/0, /*priority=*/0, false);
    ASSERT_TRUE(sgRes.hasValue()) << "beginSubgroup should succeed";
  });

  // Step 3: session A's connection drops WITHOUT closing the subgroup.
  // publishDone → onPublishDone (handle = null) + forwarder drainSubscriber.
  // Because B has an open subgroup, B stays in the forwarder
  // (receivedPublishDone_ = true) — subscriptions_ entry survives with
  // handle == nullptr.
  withSessionContext(publisherSession, [&]() {
    consumer->publishDone(
        {RequestID(0), PublishDoneStatusCode::SESSION_CLOSED, 0, "upstream disconnect"}
    );
  });

  // Step 4: session A reconnects and re-publishes the same track.
  // Before the fix this crashes (null handle->unsubscribe()).
  auto reconnectedSession = createMockSession();
  doPublishNamespace(reconnectedSession, kTestNamespace);
  auto consumer2 = doPublish(reconnectedSession, kTestTrackName);
  EXPECT_NE(consumer2, nullptr) << "re-publish after reconnect should succeed";

  removeSession(publisherSession);
  removeSession(subscriberSession);
  removeSession(reconnectedSession);
}

// ============================================================
// New Group Request (NGR) Tests
// ============================================================

namespace {
// Simple callback that records every newGroupRequested call.
struct TestNGRCallback : public MoQForwarder::Callback {
  void onEmpty(MoQForwarder*) override {}
  void newGroupRequested(MoQForwarder*, uint64_t group) override { calls.push_back(group); }
  std::vector<uint64_t> calls;
};

// Build a minimal params object carrying NEW_GROUP_REQUEST=val.
auto makeNGRParams(uint64_t val) {
  RequestUpdate upd;
  upd.params.insertParam(
      Parameter(folly::to_underlying(TrackRequestParamKey::NEW_GROUP_REQUEST), val)
  );
  return upd.params;
}
} // namespace

// Relay test: When a late-joining subscriber sends NEW_GROUP_REQUEST in its
// SUBSCRIBE, the relay forwards it upstream via REQUEST_UPDATE
TEST_F(MoQRelayTest, RelaySubscribeLateJoinerNGRForwardedUpstream) {
  auto publisherSession = createMockSession();
  auto subscriber1 = createMockSession();
  auto subscriber2 = createMockSession();

  doPublishNamespace(publisherSession, kTestNamespace);

  // Upstream SubscribeOk advertises DYNAMIC_GROUPS = true
  SubscribeOk upstreamOk;
  upstreamOk.requestID = RequestID(100);
  upstreamOk.trackAlias = TrackAlias(1);
  upstreamOk.expires = std::chrono::milliseconds(0);
  upstreamOk.groupOrder = GroupOrder::OldestFirst;
  setPublisherDynamicGroups(upstreamOk, true);

  auto upstreamHandle = std::make_shared<NiceMock<MockSubscriptionHandle>>(upstreamOk);
  ON_CALL(*upstreamHandle, requestUpdateResult())
      .WillByDefault(Return(folly::makeExpected<RequestError>(
          RequestOk{RequestID(0), TrackRequestParameters(FrameType::REQUEST_OK), {}}
      )));

  EXPECT_CALL(*publisherSession, subscribe(_, _))
      .WillOnce([&upstreamHandle](const auto& /*req*/, auto /*consumer*/) {
        return folly::coro::makeTask<Publisher::SubscribeResult>(
            folly::Expected<std::shared_ptr<SubscriptionHandle>, SubscribeError>(upstreamHandle)
        );
      });

  // First subscriber establishes the upstream subscription (no NGR)
  auto consumer1 = createMockConsumer();
  auto handle1 = subscribeToTrack(subscriber1, kTestTrackName, consumer1, RequestID(1));
  ASSERT_NE(handle1, nullptr);

  // Second subscriber includes NEW_GROUP_REQUEST=8
  auto consumer2 = createMockConsumer();
  SubscribeRequest sub2;
  sub2.fullTrackName = kTestTrackName;
  sub2.requestID = RequestID(2);
  sub2.locType = LocationType::LargestObject;
  sub2.params.insertParam(
      Parameter(folly::to_underlying(TrackRequestParamKey::NEW_GROUP_REQUEST), uint64_t(8))
  );

  // The relay must forward NGR=8 upstream via REQUEST_UPDATE
  EXPECT_CALL(*upstreamHandle, requestUpdateCalled(_)).WillOnce([](const RequestUpdate& update) {
    auto ngrValue = getFirstIntParam(update.params, TrackRequestParamKey::NEW_GROUP_REQUEST);
    ASSERT_TRUE(ngrValue.has_value());
    EXPECT_EQ(*ngrValue, 8);
  });

  std::shared_ptr<SubscriptionHandle> handle2{nullptr};
  withSessionContext(subscriber2, [&]() {
    auto task = relay_->subscribe(std::move(sub2), consumer2);
    auto res = folly::coro::blockingWait(std::move(task), exec_.get());
    ASSERT_TRUE(res.hasValue());
    handle2 = *res;
  });
  exec_->drive();

  // Register handle2 so removeSession(subscriber2) will unsubscribe it,
  // allowing the forwarder to become empty and release the upstream handle.
  getOrCreateMockState(subscriber2)->subscribeHandles.push_back(handle2);

  removeSession(publisherSession);
  removeSession(subscriber1);
  removeSession(subscriber2);
  exec_->drive();
}

// Relay test: A downstream subscriber sending REQUEST_UPDATE with
// NEW_GROUP_REQUEST causes the relay to cascade the NGR upstream
TEST_F(MoQRelayTest, RelayRequestUpdateNGRCascadedUpstream) {
  auto publisherSession = createMockSession();
  auto subscriberSession = createMockSession();

  doPublishNamespace(publisherSession, kTestNamespace);

  // Upstream SubscribeOk advertises DYNAMIC_GROUPS = true
  SubscribeOk upstreamOk;
  upstreamOk.requestID = RequestID(100);
  upstreamOk.trackAlias = TrackAlias(1);
  upstreamOk.expires = std::chrono::milliseconds(0);
  upstreamOk.groupOrder = GroupOrder::OldestFirst;
  setPublisherDynamicGroups(upstreamOk, true);

  auto upstreamHandle = std::make_shared<NiceMock<MockSubscriptionHandle>>(upstreamOk);
  ON_CALL(*upstreamHandle, requestUpdateResult())
      .WillByDefault(Return(folly::makeExpected<RequestError>(
          RequestOk{RequestID(0), TrackRequestParameters(FrameType::REQUEST_OK), {}}
      )));

  EXPECT_CALL(*publisherSession, subscribe(_, _))
      .WillOnce([&upstreamHandle](const auto& /*req*/, auto /*consumer*/) {
        return folly::coro::makeTask<Publisher::SubscribeResult>(
            folly::Expected<std::shared_ptr<SubscriptionHandle>, SubscribeError>(upstreamHandle)
        );
      });

  // Subscribe downstream session - triggers upstream subscribe
  auto consumer = createMockConsumer();
  auto handle = subscribeToTrack(subscriberSession, kTestTrackName, consumer, RequestID(1));
  ASSERT_NE(handle, nullptr);

  auto* subscriber = dynamic_cast<MoQForwarder::Subscriber*>(handle.get());
  ASSERT_NE(subscriber, nullptr);

  // The relay must cascade NGR=9 upstream via REQUEST_UPDATE
  EXPECT_CALL(*upstreamHandle, requestUpdateCalled(_)).WillOnce([](const RequestUpdate& update) {
    auto ngrValue = getFirstIntParam(update.params, TrackRequestParamKey::NEW_GROUP_REQUEST);
    ASSERT_TRUE(ngrValue.has_value());
    EXPECT_EQ(*ngrValue, 9);
  });

  // Downstream subscriber sends REQUEST_UPDATE carrying NEW_GROUP_REQUEST=9
  RequestUpdate update;
  update.requestID = RequestID(2);
  update.existingRequestID = RequestID(1);
  update.params.insertParam(
      Parameter(folly::to_underlying(TrackRequestParamKey::NEW_GROUP_REQUEST), uint64_t(9))
  );
  folly::coro::blockingWait(subscriber->requestUpdate(std::move(update)));
  exec_->drive();

  removeSession(publisherSession);
  removeSession(subscriberSession);
}

// Unit test: Subscriber::onPublishOk postprocessing
// Verifies that onPublishOk correctly updates:
// 1. Subscriber range based on PublishOk fields
// 2. shouldForward flag
// 3. NEW_GROUP_REQUEST forwarding when it passes gating checks
TEST_F(MoQRelayTest, SubscriberOnPublishOkPostprocessing) {
  auto publisherSession = createMockSession();
  auto subscriberSession = createMockSession();

  // Publish track and subscribe
  auto publishConsumer = doPublish(publisherSession, kTestTrackName);
  auto consumer = createMockConsumer();
  auto handle = subscribeToTrack(subscriberSession, kTestTrackName, consumer, RequestID(1));
  ASSERT_NE(handle, nullptr);

  // Cast to access Subscriber internals
  auto* subscriber = dynamic_cast<MoQForwarder::Subscriber*>(handle.get());
  ASSERT_NE(subscriber, nullptr);

  // Test 1: Range update from PublishOk
  // Create a PublishOk with specific start/end locations
  PublishOk pubOk1{
      RequestID(1),                // requestID
      true,                        // forward
      0,                           // subscriberPriority
      GroupOrder::OldestFirst,     // groupOrder
      LocationType::AbsoluteStart, // locType
      AbsoluteLocation{5, 0},      // start
      uint64_t(15),                // endGroup
      TrackRequestParameters(FrameType::PUBLISH_OK)
  };

  // Apply the postprocessing
  subscriber->onPublishOk(pubOk1);

  // Verify range was updated
  EXPECT_EQ(subscriber->range.start.group, 5);

  // Test 2: Forward flag update
  subscriber->shouldForward = true;
  PublishOk pubOk2{
      RequestID(2),
      false, // forward = false
      0,
      GroupOrder::OldestFirst,
      LocationType::AbsoluteStart,
      AbsoluteLocation{5, 0},
      uint64_t(15),
      TrackRequestParameters(FrameType::PUBLISH_OK)
  };
  subscriber->onPublishOk(pubOk2);
  EXPECT_FALSE(subscriber->shouldForward) << "Forward flag should be updated to false";

  subscriber->shouldForward = false;
  PublishOk pubOk3{
      RequestID(3),
      true, // forward = true
      0,
      GroupOrder::OldestFirst,
      LocationType::AbsoluteStart,
      AbsoluteLocation{5, 0},
      uint64_t(15),
      TrackRequestParameters(FrameType::PUBLISH_OK)
  };
  subscriber->onPublishOk(pubOk3);
  EXPECT_TRUE(subscriber->shouldForward) << "Forward flag should be updated to true";

  // Test 3: NEW_GROUP_REQUEST forwarding via onPublishOk
  // Enable dynamic groups and attach a callback to observe NGR fires
  PublishRequest pub;
  setPublisherDynamicGroups(pub, true);
  subscriber->forwarder.setExtensions(pub.extensions);

  auto cb = std::make_shared<TestNGRCallback>();
  subscriber->forwarder.setCallback(cb);

  // Build a PublishOk carrying NEW_GROUP_REQUEST=20
  TrackRequestParameters ngrParams(FrameType::PUBLISH_OK);
  ngrParams.insertParam(
      Parameter(folly::to_underlying(TrackRequestParamKey::NEW_GROUP_REQUEST), uint64_t(20))
  );
  PublishOk pubOk4{
      RequestID(4),
      true,
      0,
      GroupOrder::OldestFirst,
      LocationType::AbsoluteStart,
      AbsoluteLocation{10, 0},
      std::nullopt,
      std::move(ngrParams)
  };

  // onPublishOk should fire the NGR callback for group 20
  subscriber->onPublishOk(pubOk4);
  ASSERT_EQ(cb->calls.size(), 1u);
  EXPECT_EQ(cb->calls[0], 20u);
  cb->calls.clear();

  // outstanding=20: re-requesting group 20 is a no-op; group 21 fires
  subscriber->forwarder.tryProcessNewGroupRequest(makeNGRParams(20));
  EXPECT_TRUE(cb->calls.empty()) << "Group 20 already outstanding";
  subscriber->forwarder.tryProcessNewGroupRequest(makeNGRParams(21));
  ASSERT_EQ(cb->calls.size(), 1u);
  EXPECT_EQ(cb->calls[0], 21u);

  removeSession(publisherSession);
  removeSession(subscriberSession);
}

// Test: Duplicate beginSubgroup with active consumers resets them and creates
// new ones.
// Sequence: publish, 2 subscribers, beginSubgroup, beginSubgroup again ->
// first consumers get reset, both subscribers get new consumers.
TEST_F(MoQRelayTest, DuplicateSubgroupReplacesActiveConsumers) {
  auto publisherSession = createMockSession();
  auto sub1 = createMockSession();
  auto sub2 = createMockSession();

  auto mockConsumer1 = createMockConsumer();
  auto mockConsumer2 = createMockConsumer();

  auto sg1v1 = createMockSubgroupConsumer();
  auto sg2v1 = createMockSubgroupConsumer();
  auto sg1v2 = createMockSubgroupConsumer();
  auto sg2v2 = createMockSubgroupConsumer();

  // First beginSubgroup gives v1 consumers; second call gives v2 consumers
  EXPECT_CALL(*mockConsumer1, beginSubgroup(0, 0, _, _))
      .WillOnce([&](uint64_t, uint64_t, uint8_t, bool) {
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(sg1v1);
      })
      .WillOnce([&](uint64_t, uint64_t, uint8_t, bool) {
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(sg1v2);
      });
  EXPECT_CALL(*mockConsumer2, beginSubgroup(0, 0, _, _))
      .WillOnce([&](uint64_t, uint64_t, uint8_t, bool) {
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(sg2v1);
      })
      .WillOnce([&](uint64_t, uint64_t, uint8_t, bool) {
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(sg2v2);
      });

  // v1 consumers should be reset when duplicate arrives
  EXPECT_CALL(*sg1v1, reset(ResetStreamErrorCode::CANCELLED)).Times(1);
  EXPECT_CALL(*sg2v1, reset(ResetStreamErrorCode::CANCELLED)).Times(1);
  // v2 consumers should not be reset during duplicate handling; they will be
  // closed cleanly via endOfSubgroup before teardown
  EXPECT_CALL(*sg1v2, reset(_)).Times(0);
  EXPECT_CALL(*sg2v2, reset(_)).Times(0);

  auto publishConsumer = doPublish(publisherSession, kTestTrackName);
  subscribeToTrack(sub1, kTestTrackName, mockConsumer1, RequestID(1));
  subscribeToTrack(sub2, kTestTrackName, mockConsumer2, RequestID(2));

  auto sgForwarder1 = publishConsumer->beginSubgroup(0, 0, 0);
  EXPECT_TRUE(sgForwarder1.hasValue());

  // Duplicate beginSubgroup - should reset v1 consumers and return new
  // forwarder
  auto sgForwarder2 = publishConsumer->beginSubgroup(0, 0, 0);
  EXPECT_TRUE(sgForwarder2.hasValue());
  EXPECT_NE(sgForwarder1.value(), sgForwarder2.value());

  // Close the new subgroup cleanly before teardown to avoid reset during
  // cleanup
  EXPECT_TRUE(sgForwarder2.value()->endOfSubgroup().hasValue());

  removeSession(publisherSession);
  removeSession(sub1);
  removeSession(sub2);
}

// Test: Duplicate beginSubgroup after all subscribers have stop_sending'd
// returns CANCELLED to propagate the signal back to the publisher.
TEST_F(MoQRelayTest, DuplicateSubgroupCancelledWhenNoActiveConsumers) {
  auto publisherSession = createMockSession();
  auto subscriber = createMockSession();

  auto mockConsumer = createMockConsumer();
  auto mockSg = std::make_shared<NiceMock<MockSubgroupConsumer>>();

  EXPECT_CALL(*mockConsumer, beginSubgroup(0, 0, _, _))
      .WillOnce([&](uint64_t, uint64_t, uint8_t, bool) {
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(mockSg);
      });

  // Subscriber's object() returns CANCELLED to simulate stop_sending
  EXPECT_CALL(*mockSg, object(_, _, _, _))
      .WillOnce(
          Return(folly::makeUnexpected(MoQPublishError(MoQPublishError::CANCELLED, "stop sending")))
      );

  auto publishConsumer = doPublish(publisherSession, kTestTrackName);
  subscribeToTrack(subscriber, kTestTrackName, mockConsumer, RequestID(1));

  auto sgRes = publishConsumer->beginSubgroup(0, 0, 0);
  ASSERT_TRUE(sgRes.hasValue());
  auto sg = sgRes.value();

  // Trigger stop_sending tombstone via CANCELLED error from object()
  sg->object(0, nullptr, {}, false);

  // Duplicate beginSubgroup - all consumers tombstoned, should return CANCELLED
  auto dupRes = publishConsumer->beginSubgroup(0, 0, 0);
  EXPECT_TRUE(dupRes.hasError());
  EXPECT_EQ(dupRes.error().code, MoQPublishError::CANCELLED);

  removeSession(publisherSession);
  removeSession(subscriber);
}

// Test: Duplicate beginSubgroup with partial stop_sending - active subscriber
// gets reset and new consumer; tombstoned subscriber is skipped.
TEST_F(MoQRelayTest, DuplicateSubgroupSkipsTombstonedSubscriber) {
  auto publisherSession = createMockSession();
  auto subA = createMockSession();
  auto subB = createMockSession();

  auto consumerA = createMockConsumer();
  auto consumerB = createMockConsumer();

  auto sgAv1 = createMockSubgroupConsumer();
  auto sgBv1 = createMockSubgroupConsumer();
  auto sgAv2 = createMockSubgroupConsumer();

  // First beginSubgroup: both A and B get consumers
  EXPECT_CALL(*consumerA, beginSubgroup(0, 0, _, _))
      .WillOnce([&](uint64_t, uint64_t, uint8_t, bool) {
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(sgAv1);
      })
      .WillOnce([&](uint64_t, uint64_t, uint8_t, bool) {
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(sgAv2);
      });
  EXPECT_CALL(*consumerB, beginSubgroup(0, 0, _, _))
      .WillOnce([&](uint64_t, uint64_t, uint8_t, bool) {
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(sgBv1);
      });

  // object() is forwarded to both A and B; sub A succeeds, sub B returns
  // CANCELLED to simulate stop_sending
  EXPECT_CALL(*sgAv1, object(_, _, _, _))
      .WillOnce(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
  EXPECT_CALL(*sgBv1, object(_, _, _, _))
      .WillOnce(
          Return(folly::makeUnexpected(MoQPublishError(MoQPublishError::CANCELLED, "stop sending")))
      );

  // On duplicate: sub A's v1 consumer gets reset; sub B is tombstoned (no
  // reset)
  EXPECT_CALL(*sgAv1, reset(ResetStreamErrorCode::CANCELLED)).Times(1);
  EXPECT_CALL(*sgBv1, reset(_)).Times(0);
  EXPECT_CALL(*sgAv2, reset(_)).Times(0);

  auto publishConsumer = doPublish(publisherSession, kTestTrackName);
  subscribeToTrack(subA, kTestTrackName, consumerA, RequestID(1));
  subscribeToTrack(subB, kTestTrackName, consumerB, RequestID(2));

  auto sgForwarder1 = publishConsumer->beginSubgroup(0, 0, 0);
  ASSERT_TRUE(sgForwarder1.hasValue());

  // Trigger tombstone for sub B via CANCELLED from object()
  sgForwarder1.value()->object(0, nullptr, {}, false);

  // Duplicate beginSubgroup: sub A gets reset+new, sub B is skipped
  // (tombstoned)
  auto sgForwarder2 = publishConsumer->beginSubgroup(0, 0, 0);
  EXPECT_TRUE(sgForwarder2.hasValue());
  EXPECT_NE(sgForwarder1.value(), sgForwarder2.value());

  // Close the new subgroup cleanly before teardown
  EXPECT_TRUE(sgForwarder2.value()->endOfSubgroup().hasValue());

  removeSession(publisherSession);
  removeSession(subA);
  removeSession(subB);
}

// ============================================================
// Publish Replaces Subscribe Tests
// ============================================================

// Regression test: When a PUBLISH replaces a subscribe-path subscription, the
// old forwarder's subscribers must receive publishDone, and the new
// publish-path subscription must be fully functional (accepting data from the
// new publisher).
TEST_F(MoQRelayTest, PublishReplacesSubscribeDrainsOldAndServesNew) {
  auto publisherSession = createMockSession();
  auto subscriberSession = createMockSession();

  doPublishNamespace(publisherSession, kTestNamespace);

  // Set up upstream subscribe that succeeds
  SubscribeOk upstreamOk;
  upstreamOk.requestID = RequestID(1);
  upstreamOk.trackAlias = TrackAlias(1);
  upstreamOk.expires = std::chrono::milliseconds(0);
  upstreamOk.groupOrder = GroupOrder::OldestFirst;

  EXPECT_CALL(*publisherSession, subscribe(_, _))
      .WillOnce([upstreamOk](const auto& /*req*/, auto /*consumer*/) {
        auto handle = std::make_shared<NiceMock<MockSubscriptionHandle>>(upstreamOk);
        return folly::coro::makeTask<Publisher::SubscribeResult>(
            folly::Expected<std::shared_ptr<SubscriptionHandle>, SubscribeError>(handle)
        );
      });

  // Subscribe to the track (creates subscribe-path subscription)
  auto oldConsumer = createMockConsumer();
  bool publishDoneReceived = false;
  EXPECT_CALL(*oldConsumer, publishDone(_)).WillOnce([&publishDoneReceived](const PublishDone&) {
    publishDoneReceived = true;
    return folly::makeExpected<MoQPublishError>(folly::unit);
  });
  auto handle = subscribeToTrack(
      subscriberSession,
      kTestTrackName,
      oldConsumer,
      RequestID(1),
      /*addToState=*/false
  );
  ASSERT_NE(handle, nullptr);

  // PUBLISH arrives for the same track — replaces subscribe-path subscription
  auto publishConsumer = doPublish(publisherSession, kTestTrackName);
  ASSERT_NE(publishConsumer, nullptr);

  // Old subscriber must have been drained
  EXPECT_TRUE(publishDoneReceived) << "Old subscribe-path subscriber should receive publishDone";

  // New publish-path subscription should be functional: subscribe a new
  // downstream consumer and verify it receives data from the publisher
  auto newConsumer = createMockConsumer();
  auto sg = createMockSubgroupConsumer();
  EXPECT_CALL(*newConsumer, beginSubgroup(0, 0, _, _))
      .WillOnce([&sg](uint64_t, uint64_t, uint8_t, bool) {
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(sg);
      });
  EXPECT_CALL(*sg, endOfSubgroup()).WillOnce(Return(folly::unit));

  subscribeToTrack(subscriberSession, kTestTrackName, newConsumer, RequestID(2));

  auto sgRes = publishConsumer->beginSubgroup(0, 0, 0);
  ASSERT_TRUE(sgRes.hasValue());
  EXPECT_TRUE(sgRes.value()->endOfSubgroup().hasValue());

  removeSession(publisherSession);
  removeSession(subscriberSession);
}

// Test: forwardChanged must not crash when called after the publisher has
// terminated (onPublishDone clears handle/upstream). We trigger forwardChanged
// via Subscriber::requestUpdate changing forward from true→false (1→0
// transition). The subscriber survives drain because it has an open subgroup.
TEST_F(MoQRelayTest, ForwardChangedAfterPublisherTermination) {
  auto publisherSession = createMockSession();
  auto subSession = createMockSession();

  doPublishNamespace(publisherSession, kTestNamespace);
  auto publishConsumer = doPublish(publisherSession, kTestTrackName);

  // Subscriber with forward=true (default)
  auto consumer = createMockConsumer();
  auto handle = subscribeToTrack(subSession, kTestTrackName, consumer, RequestID(0));
  ASSERT_NE(handle, nullptr);

  // Begin a subgroup so the subscriber has open subgroups and survives drain
  auto sg = createMockSubgroupConsumer();
  EXPECT_CALL(*consumer, beginSubgroup(0, 0, _, _))
      .WillOnce([&sg](uint64_t, uint64_t, uint8_t, bool) {
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(sg);
      });
  auto subgroupRes = publishConsumer->beginSubgroup(0, 0, 0);
  ASSERT_TRUE(subgroupRes.hasValue());

  // Publisher terminates — onPublishDone clears handle/upstream.
  // forwarder->publishDone sets draining and calls drainSubscriber, but the
  // subscriber has an open subgroup so it stays (receivedPublishDone_=true).
  EXPECT_CALL(*consumer, publishDone(_))
      .WillOnce(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
  publishConsumer->publishDone(
      {RequestID(0), PublishDoneStatusCode::SUBSCRIPTION_ENDED, 0, "publisher ended"}
  );

  // Subscriber sends requestUpdate changing forward from true→false.
  // This calls removeForwardingSubscriber → forwardingSubscribers_ 1→0 →
  // forwardChanged on relay callback. forwardChanged accesses
  // subscription.upstream which was nulled by onPublishDone → crash.
  RequestUpdate update;
  update.requestID = RequestID(0);
  update.forward = false;
  auto task = handle->requestUpdate(std::move(update));
  auto res = folly::coro::blockingWait(std::move(task), exec_.get());
  EXPECT_TRUE(res.hasValue());

  // Clean up: reset the subgroup so subscriber can be fully removed
  EXPECT_CALL(*sg, reset(_)).Times(1);
  subgroupRes.value()->reset(ResetStreamErrorCode::CANCELLED);

  removeSession(publisherSession);
  removeSession(subSession);
}

// Test: fetch fallback to subscriptions_ after publisher termination must not
// crash. When findPublishNamespaceSession returns null (no publishNamespace),
// fetch falls back to subscriptions_. After onPublishDone, upstream is null
// but the subscription entry remains if the forwarder has subscribers.
TEST_F(MoQRelayTest, FetchAfterPublisherTermination) {
  auto publisherSession = createMockSession();
  auto subSession = createMockSession();
  auto fetchSession = createMockSession();

  // Publish WITHOUT publishNamespace so findPublishNamespaceSession returns null
  // and fetch falls back to subscriptions_
  auto publishConsumer = doPublish(publisherSession, kTestTrackName, /*addToState=*/false);

  // Subscriber with open subgroup so subscription survives publisher drain
  auto consumer = createMockConsumer();
  auto sg = createMockSubgroupConsumer();
  EXPECT_CALL(*consumer, beginSubgroup(0, 0, _, _))
      .WillOnce([&sg](uint64_t, uint64_t, uint8_t, bool) {
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(sg);
      });
  auto handle = subscribeToTrack(subSession, kTestTrackName, consumer, RequestID(0));
  ASSERT_NE(handle, nullptr);

  // Begin subgroup to keep subscriber alive
  auto subgroupRes = publishConsumer->beginSubgroup(0, 0, 0);
  ASSERT_TRUE(subgroupRes.hasValue());

  // Publisher terminates — clears upstream but subscription stays
  EXPECT_CALL(*consumer, publishDone(_))
      .WillOnce(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
  publishConsumer->publishDone(
      {RequestID(0), PublishDoneStatusCode::SUBSCRIPTION_ENDED, 0, "publisher ended"}
  );

  // Fetch from a different session — falls back to subscriptions_, gets null
  // upstream, then crashes at line 1011 dereferencing null upstreamSession
  Fetch fetch(RequestID(0), kTestTrackName, AbsoluteLocation{0, 0}, AbsoluteLocation{1, 0});
  auto fetchConsumer = std::make_shared<NiceMock<MockFetchConsumer>>();
  withSessionContext(fetchSession, [&]() {
    auto task = relay_->fetch(std::move(fetch), fetchConsumer);
    auto res = folly::coro::blockingWait(std::move(task), exec_.get());
    // Should return an error, not crash
    EXPECT_FALSE(res.hasValue());
    EXPECT_EQ(res.error().errorCode, FetchErrorCode::TRACK_NOT_EXIST);
  });

  // Clean up
  EXPECT_CALL(*sg, reset(_)).Times(1);
  subgroupRes.value()->reset(ResetStreamErrorCode::CANCELLED);
  handle->unsubscribe();
  removeSession(publisherSession);
  removeSession(subSession);
  removeSession(fetchSession);
}

// Bug: when a subscriber with forward=true joins a namespace whose track
// forwarder is empty, the relay fires REQUEST_UPDATE twice — once explicitly
// at the if(forwarder->empty()) site and once via forwardChanged() when
// addSubscriber() increments numForwardingSubscribers from 0 to 1.
TEST_F(MoQRelayTest, SubscribeNs_ForwardTrue_EmptyForwarder_SingleRequestUpdate) {
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

// Bug: when a subscriber with forward=false joins a namespace whose track
// forwarder is empty, the relay fires a spurious REQUEST_UPDATE(forward=false)
// at the if(forwarder->empty()) site — even though the upstream is already at
// forward=false (set by publish() which found no subscribers).
TEST_F(MoQRelayTest, SubscribeNs_ForwardFalse_EmptyForwarder_NoRequestUpdate) {
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

// Bug: when a second subscriber with forward=true joins an existing PUBLISH-path
// subscription (causing a 0→1 forwarding transition), the relay fires REQUEST_UPDATE
// twice — once via forwardChanged() (which fires synchronously inside addSubscriber
// via addForwardingSubscriber) and once via the explicit block at the end of the
// subscribe() else-branch. Analogous to the subscribeNamespace bug fixed in this PR.
TEST_F(MoQRelayTest, Subscribe_SecondForwardingSubscriber_SingleRequestUpdate) {
  auto pubSession = createMockSession();
  doPublishNamespace(pubSession, kTestNamespace);
  auto mockHandle = makePublishHandle();
  doPublishWithHandle(pubSession, kTestTrackName, mockHandle);

  // S1 joins with forward=false — no REQUEST_UPDATE expected (no forwarding change).
  auto s1 = createMockSession();
  setupPublishSucceeds(s1);
  {
    SubscribeRequest sub;
    sub.fullTrackName = kTestTrackName;
    sub.requestID = RequestID(1);
    sub.locType = LocationType::LargestObject;
    sub.forward = false;
    withSessionContext(s1, [&]() {
      auto res = folly::coro::blockingWait(
          relay_->subscribe(std::move(sub), createMockConsumer()),
          exec_.get()
      );
      EXPECT_TRUE(res.hasValue());
      if (res.hasValue()) {
        getOrCreateMockState(s1)->subscribeHandles.push_back(*res);
      }
    });
  }
  for (int i = 0; i < 3; i++) {
    exec_->drive();
  }

  // Now expect exactly ONE REQUEST_UPDATE(forward=true) when S2 joins.
  // Before the fix this fires TWICE (forwardChanged + explicit block).
  EXPECT_CALL(*mockHandle, requestUpdateCalled(_)).Times(1).WillOnce([](const RequestUpdate& u) {
    ASSERT_TRUE(u.forward.has_value());
    EXPECT_TRUE(*u.forward);
  });

  auto s2 = createMockSession();
  setupPublishSucceeds(s2);
  {
    SubscribeRequest sub;
    sub.fullTrackName = kTestTrackName;
    sub.requestID = RequestID(2);
    sub.locType = LocationType::LargestObject;
    sub.forward = true;
    withSessionContext(s2, [&]() {
      auto res = folly::coro::blockingWait(
          relay_->subscribe(std::move(sub), createMockConsumer()),
          exec_.get()
      );
      EXPECT_TRUE(res.hasValue());
      if (res.hasValue()) {
        getOrCreateMockState(s2)->subscribeHandles.push_back(*res);
      }
    });
  }
  for (int i = 0; i < 5; i++) {
    exec_->drive();
  }

  // Verify before cleanup (cleanup legitimately sends forward=false).
  ASSERT_TRUE(testing::Mock::VerifyAndClearExpectations(mockHandle.get()));

  removeSession(s2);
  removeSession(s1);
  removeSession(pubSession);
  for (int i = 0; i < 3; i++) {
    exec_->drive();
  }
}

// ============================================================
// Peer relay namespace loop prevention tests
// ============================================================

// Regression test: when a peer relay reconnects and subscribes to our
// namespaces, we must not echo back namespaces that originally came FROM that
// peer — doing so overwrites the real publisher in the namespace tree and
// breaks data flow for downstream subscribers.
//
// Scenario (relay acts as sg-sin-2-1, upstream is jp-osa-1):
//   1. jp-osa-1 (session1) announces namespace NS to our relay.
//   2. session1 drops (old QUIC connection not yet reaped).
//   3. jp-osa-1 reconnects (session2) and sends a peer SUBSCRIBE_NAMESPACE.
//   4. Bug: the relay walks its tree, finds NS with sourceSession=session1,
//      session1 != session2, and delivers NS back to session2 — echo loop.
//   5. Fix: NS is tagged with sourcePeerID="jp-osa-1" so the peer ID check
//      suppresses the delivery regardless of session identity.
//
// Production relays negotiate draft-16 (empty prefix allowed).  The delivery
// path for draft-16 is synchronous: namespacePublishHandle->namespaceMsg().
TEST_F(MoQRelayTest, PeerNamespaceNotEchoedBackOnReconnect) {
  // Relay must have a relayID for peer detection to activate.
  relay_ = std::make_shared<MoqxRelay>(config::CacheConfig{.maxCachedTracks = 0}, "sg-sin-2-1");
  relay_->setAllowedNamespacePrefix(kAllowedPrefix);

  // Step 1: session1 is the peer's old (unreaped) connection.  Inject NS as
  // if it arrived from peer "jp-osa-1" via the bridge handle.
  auto session1 = createMockSession();
  auto bridgeHandle = makeNamespaceBridgeHandle(relay_, session1, "jp-osa-1");
  bridgeHandle->namespaceMsg(kTestNamespace);
  ASSERT_EQ(relay_->findPublishNamespaceSessions(kTestNamespace).size(), 1u);

  // Step 2: jp-osa-1 reconnects as session2 and sends a peer SUBSCRIBE_NAMESPACE.
  // Peer-to-peer sessions negotiate draft-16 (empty prefix is a 16+ feature).
  auto session2 = createMockSession();
  ON_CALL(*session2, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft16)));

  // The relay delivers existing namespaces via namespacePublishHandle->namespaceMsg
  // (draft-16 synchronous path).  Use a mock handle to detect any echo.
  auto nsHandle = std::make_shared<NiceMock<MockNamespacePublishHandle>>();
  bool echoedBack = false;
  ON_CALL(*nsHandle, namespaceMsg(_)).WillByDefault([&echoedBack](const TrackNamespace&) {
    echoedBack = true;
  });

  withSessionContext(session2, [&]() {
    // makePeerSubNs("jp-osa-1") carries jp-osa-1's relay ID in the auth token.
    auto task = relay_->subscribeNamespace(makePeerSubNs("jp-osa-1"), nsHandle);
    folly::coro::blockingWait(std::move(task), exec_.get());
  });

  EXPECT_FALSE(echoedBack) << "Relay echoed a peer's own namespace back to it on reconnect";

  removeSession(session1);
  removeSession(session2);
}

// Complement: namespaces from LOCAL publishers (not from the peer) must still
// be delivered when that peer subscribes.
TEST_F(MoQRelayTest, LocalNamespaceDeliveredToPeerOnReconnect) {
  relay_ = std::make_shared<MoqxRelay>(config::CacheConfig{.maxCachedTracks = 0}, "sg-sin-2-1");
  relay_->setAllowedNamespacePrefix(kAllowedPrefix);

  // Local publisher session announces kTestNamespace.
  auto localPublisher = createMockSession();
  doPublishNamespace(localPublisher, kTestNamespace);

  // Peer "jp-osa-1" subscribes with a draft-16 session.
  auto peerSession = createMockSession();
  ON_CALL(*peerSession, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft16)));

  auto nsHandle = std::make_shared<NiceMock<MockNamespacePublishHandle>>();
  bool delivered = false;
  ON_CALL(*nsHandle, namespaceMsg(_)).WillByDefault([&delivered](const TrackNamespace&) {
    delivered = true;
  });

  withSessionContext(peerSession, [&]() {
    auto task = relay_->subscribeNamespace(makePeerSubNs("jp-osa-1"), nsHandle);
    folly::coro::blockingWait(std::move(task), exec_.get());
  });
  EXPECT_TRUE(delivered) << "Relay failed to deliver a local namespace to a peer subscriber";

  removeSession(localPublisher);
  removeSession(peerSession);
}

// A mock session that simulates a peer announcing peerNs when the relay
// calls subscribeNamespace on it (the reciprocal leg).
//
// handleOut receives the bridge handle so the test can control its lifetime
// independently of the session object — mirroring how SubNsStreamCallback
// lives in a coroutine frame that is separate from (but tied to) the session.
class PeerAnnounceSession : public NiceMock<MockMoQSession> {
public:
  explicit PeerAnnounceSession(
      std::shared_ptr<MoQExecutor> exec,
      TrackNamespace peerNs,
      std::shared_ptr<Publisher::NamespacePublishHandle>& handleOut
  )
      : NiceMock<MockMoQSession>(std::move(exec)), peerNs_(std::move(peerNs)),
        handleOut_(handleOut) {}

  folly::coro::Task<Publisher::SubscribeNamespaceResult> subscribeNamespace(
      SubscribeNamespace subNs,
      std::shared_ptr<Publisher::NamespacePublishHandle> handle
  ) override {
    handleOut_ = handle;
    if (handle) {
      handle->namespaceMsg(peerNs_);
    }
    co_return std::make_shared<NiceMock<MockSubscribeNamespaceHandle>>(
        SubscribeNamespaceOk{subNs.requestID}
    );
  }

private:
  TrackNamespace peerNs_;
  std::shared_ptr<Publisher::NamespacePublishHandle>& handleOut_;
};

// Full production-path regression test: verifies that the peerID stored on
// namespace nodes via the reciprocal bridge handle is the INCOMING peer's relay
// ID (not our own relayID_).
//
// Bug: makeNamespaceBridgeHandle was passed relayID_ ("sg-sin-2-1") instead of
// incomingPeerID ("jp-osa-1"), so nodes got sourcePeerID="sg-sin-2-1".  On
// reconnect the check "sg-sin-2-1" != "jp-osa-1" is true and the namespace
// was echoed back — the loop survived.
//
// Unlike PeerNamespaceNotEchoedBackOnReconnect (which injects the namespace
// directly via makeNamespaceBridgeHandle), this test goes through the full
// relay_->subscribeNamespace() production path so the bug in the call-site is
// exercised.
TEST_F(MoQRelayTest, PeerNamespaceNotEchoedBack_FullProductionPath) {
  relay_ = std::make_shared<MoqxRelay>(config::CacheConfig{.maxCachedTracks = 0}, "sg-sin-2-1");
  relay_->setAllowedNamespacePrefix(kAllowedPrefix);

  // Step 1: jp-osa-1 connects as session1.  It will announce kTestNamespace
  // to us via the reciprocal bridge handle when we subscribe to it.
  // bridgeHandle1 is held here (not inside the session) to model the production
  // lifetime: the handle lives in the coroutine frame, separate from the session.
  std::shared_ptr<Publisher::NamespacePublishHandle> bridgeHandle1;
  auto session1 = std::make_shared<PeerAnnounceSession>(exec_, kTestNamespace, bridgeHandle1);
  ON_CALL(*session1, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft16)));
  getOrCreateMockState(session1);

  auto nsHandle1 = std::make_shared<NiceMock<MockNamespacePublishHandle>>();
  withSessionContext(session1, [&]() {
    auto task = relay_->subscribeNamespace(makePeerSubNs("jp-osa-1"), nsHandle1);
    folly::coro::blockingWait(std::move(task), exec_.get());
  });

  // kTestNamespace should now be in the tree with sourcePeerID="jp-osa-1".
  ASSERT_EQ(relay_->findPublishNamespaceSessions(kTestNamespace).size(), 1u);

  // Step 2: jp-osa-1 reconnects as session2 and re-subscribes.
  // kTestNamespace must NOT be echoed back.
  auto session2 = createMockSession();
  ON_CALL(*session2, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft16)));

  auto nsHandle2 = std::make_shared<NiceMock<MockNamespacePublishHandle>>();
  bool echoedBack = false;
  ON_CALL(*nsHandle2, namespaceMsg(_)).WillByDefault([&echoedBack](const TrackNamespace&) {
    echoedBack = true;
  });

  withSessionContext(session2, [&]() {
    auto task = relay_->subscribeNamespace(makePeerSubNs("jp-osa-1"), nsHandle2);
    folly::coro::blockingWait(std::move(task), exec_.get());
  });

  EXPECT_FALSE(echoedBack) << "Relay echoed peer namespace back on reconnect (production path)";

  // Simulate session1's coroutine frame being destroyed before session cleanup.
  bridgeHandle1.reset();
  removeSession(session1);
  removeSession(session2);
}

// ============================================================
// Bridge handle cleanup tests
// ============================================================

// Regression test: when a bridge handle is destroyed (ungraceful session close)
// without graceful namespaceDoneMsg calls, tree entries it created must be
// cleaned up so stale sourceSession shared_ptrs don't keep dead session objects
// alive and downstream subscribers receive NAMESPACE_DONE.
TEST_F(MoQRelayTest, BridgeHandleDestructorCleansUpNamespaces) {
  auto upstreamSession = createMockSession();

  // Simulate the bridge path: create a handle and announce a namespace through
  // it, as happens when the relay subscribes to an upstream/peer.
  auto bridgeHandle = makeNamespaceBridgeHandle(relay_, upstreamSession, /*peerID=*/{});
  bridgeHandle->namespaceMsg(kTestNamespace);
  ASSERT_EQ(relay_->findPublishNamespaceSessions(kTestNamespace).size(), 1u);

  // Drop the bridge handle without graceful namespaceDoneMsg — simulates
  // ungraceful session close destroying SubNsStreamCallback.
  bridgeHandle.reset();

  // Tree entry must be gone.
  EXPECT_EQ(relay_->findPublishNamespaceSessions(kTestNamespace).size(), 0u)
      << "Stale namespace tree entry persists after bridge handle destruction";

  removeSession(upstreamSession);
}

// Verify that when a new publisher takes over a namespace before the old
// bridge handle is destroyed, the stale handle's destructor does NOT evict
// the new publisher's entry (doPublishNamespaceDone guards on sourceSession).
TEST_F(MoQRelayTest, BridgeHandleDestructorDoesNotEvictNewPublisher) {
  auto session1 = createMockSession();
  auto session2 = createMockSession();

  // session1 announces NS via bridge handle.
  auto bridgeHandle1 = makeNamespaceBridgeHandle(relay_, session1, /*peerID=*/{});
  bridgeHandle1->namespaceMsg(kTestNamespace);
  ASSERT_EQ(relay_->findPublishNamespaceSessions(kTestNamespace).size(), 1u);

  // session2 takes over NS (conflict path evicts session1, sets sourceSession=session2).
  auto bridgeHandle2 = makeNamespaceBridgeHandle(relay_, session2, /*peerID=*/{});
  bridgeHandle2->namespaceMsg(kTestNamespace);
  ASSERT_EQ(relay_->findPublishNamespaceSessions(kTestNamespace).size(), 1u);

  // Now session1's handle is destroyed (ungraceful close detected late).
  // It must NOT evict session2's entry.
  bridgeHandle1.reset();

  EXPECT_EQ(relay_->findPublishNamespaceSessions(kTestNamespace).size(), 1u)
      << "Stale bridge handle destructor evicted the new publisher's entry";

  bridgeHandle2.reset();
  removeSession(session1);
  removeSession(session2);
}

// Regression test: publisher reconnects while a subscribe coroutine is
// suspended at co_await upstreamSession->subscribe().  The reconnect
// (doPublishNamespace for publisher 2) erases the subscribe-path subscriptions_
// entry (upstream == publisherSession1 == nodePtr->sourceSession), then
// publish() for the same FTN creates a new entry whose promise is already
// satisfied.  The subscribe scope guard then calls setException() on the
// already-satisfied promise → folly::PromiseAlreadySatisfied →
// ScopeGuardImplBase::terminate() → std::terminate (exit code 139).
//
// Without the fix: crashes.  With the fix: subscribe returns an error cleanly.
TEST_F(MoQRelayTest, PublishReconnectDuringSubscribeScopeGuardCrash) {
  auto publisherSession1 = createMockSession();
  auto publisherSession2 = createMockSession();
  auto subscriberSession = createMockSession();

  // Publisher 1 announces the namespace so subscribe() will find it as the
  // upstream session and call publisherSession1->subscribe().
  PublishNamespace pn;
  pn.trackNamespace = kTestNamespace;
  relay_->doPublishNamespace(pn, publisherSession1, nullptr);

  // Configure publisher 1's subscribe() mock to simulate publisher reconnect
  // inline (the relay calls this during co_await upstreamSession->subscribe()):
  //   1. Publisher 2 re-announces the namespace — doPublishNamespace erases the
  //      subscribe-path subscriptions_ entry because its upstream field equals
  //      publisherSession1 == nodePtr->sourceSession.
  //   2. Publisher 2 publishes the FTN — publish() emplaces a new subscriptions_
  //      entry and immediately calls rsub.promise.setValue(folly::unit).
  //   3. Returns an error, simulating the old session being cancelled.
  // After the mock returns, the subscribe error path fires the scope guard which
  // does subscriptions_.find(trackName), finds the new publish-path entry, and
  // calls it->second.promise.setException() on an already-satisfied promise →
  // PromiseAlreadySatisfied → terminate().
  std::shared_ptr<TrackConsumer> pub2Consumer;
  EXPECT_CALL(*publisherSession1, subscribe(_, _))
      .WillOnce(
          [this, publisherSession2, &pub2Consumer](SubscribeRequest, std::shared_ptr<TrackConsumer>)
              -> folly::coro::Task<Publisher::SubscribeResult> {
            // Step 1: publisher 2 takes over the namespace.
            PublishNamespace pn2;
            pn2.trackNamespace = kTestNamespace;
            relay_->doPublishNamespace(pn2, publisherSession2, nullptr);

            // Step 2: publisher 2 publishes the FTN, creating a new
            // subscriptions_ entry with the promise already satisfied.
            {
              folly::RequestContextScopeGuard ctx;
              folly::RequestContext::get()->setContextData(
                  sessionRequestToken(),
                  std::make_unique<MoQSession::MoQSessionRequestData>(publisherSession2)
              );
              PublishRequest pub;
              pub.fullTrackName = kTestTrackName;
              auto res = relay_->publish(std::move(pub), createMockSubscriptionHandle());
              EXPECT_TRUE(res.hasValue()) << "publish in mock unexpectedly failed";
              if (res.hasValue()) {
                pub2Consumer = res->consumer;
              }
            }

            // Step 3: return error — simulates the old upstream session being cancelled.
            co_return folly::makeUnexpected(SubscribeError{
                RequestID(0),
                SubscribeErrorCode::INTERNAL_ERROR,
                "upstream session cancelled"
            });
          }
      );

  withSessionContext(subscriberSession, [&]() {
    SubscribeRequest sub;
    sub.fullTrackName = kTestTrackName;
    sub.requestID = RequestID(1);
    sub.locType = LocationType::LargestObject;
    auto result = folly::coro::blockingWait(
        relay_->subscribe(std::move(sub), createMockConsumer()),
        exec_.get()
    );
    // With the fix: subscribe returns an error without crashing.
    // Without the fix: std::terminate is called before this assertion runs.
    EXPECT_FALSE(result.hasValue()) << "subscribe should have failed (upstream cancelled)";
  });

  if (pub2Consumer) {
    pub2Consumer->publishDone(
        {RequestID(0), PublishDoneStatusCode::SESSION_CLOSED, 0, "test cleanup"}
    );
  }
  relay_->doPublishNamespaceDone(kTestNamespace, publisherSession2);
}

// Same reconnect scenario but the upstream subscribe returns OK instead of an
// error.  Without the fix, the crash moves from the scope guard to the success
// path: after g.dismiss(), subscriptions_.find() returns the new publish-path
// entry (promise already satisfied), and rsub.promise.setValue() throws
// PromiseAlreadySatisfied, which propagates as an unhandled coroutine exception.
// With the fix: subscribe returns SUBSCRIBE_ERROR "publisher reconnected".
TEST_F(MoQRelayTest, PublishReconnectDuringSubscribeSuccessPathCrash) {
  auto publisherSession1 = createMockSession();
  auto publisherSession2 = createMockSession();
  auto subscriberSession = createMockSession();

  PublishNamespace pn;
  pn.trackNamespace = kTestNamespace;
  relay_->doPublishNamespace(pn, publisherSession1, nullptr);

  std::shared_ptr<TrackConsumer> pub2Consumer;
  EXPECT_CALL(*publisherSession1, subscribe(_, _))
      .WillOnce(
          [this,
           publisherSession2,
           &pub2Consumer](SubscribeRequest subReq, std::shared_ptr<TrackConsumer>)
              -> folly::coro::Task<Publisher::SubscribeResult> {
            PublishNamespace pn2;
            pn2.trackNamespace = kTestNamespace;
            relay_->doPublishNamespace(pn2, publisherSession2, nullptr);

            {
              folly::RequestContextScopeGuard ctx;
              folly::RequestContext::get()->setContextData(
                  sessionRequestToken(),
                  std::make_unique<MoQSession::MoQSessionRequestData>(publisherSession2)
              );
              PublishRequest pub;
              pub.fullTrackName = kTestTrackName;
              auto res = relay_->publish(std::move(pub), createMockSubscriptionHandle());
              EXPECT_TRUE(res.hasValue()) << "publish in mock unexpectedly failed";
              if (res.hasValue()) {
                pub2Consumer = res->consumer;
              }
            }

            // Return success — the crash moves to rsub.promise.setValue() in the
            // success path (after g.dismiss()), which finds the new publish-path
            // entry whose promise is already satisfied.
            SubscribeOk ok;
            ok.requestID = subReq.requestID;
            ok.trackAlias = TrackAlias(subReq.requestID.value);
            ok.expires = std::chrono::milliseconds(0);
            ok.groupOrder = GroupOrder::OldestFirst;
            co_return std::make_shared<NiceMock<MockSubscriptionHandle>>(std::move(ok));
          }
      );

  withSessionContext(subscriberSession, [&]() {
    SubscribeRequest sub;
    sub.fullTrackName = kTestTrackName;
    sub.requestID = RequestID(1);
    sub.locType = LocationType::LargestObject;
    auto result = folly::coro::blockingWait(
        relay_->subscribe(std::move(sub), createMockConsumer()),
        exec_.get()
    );
    EXPECT_FALSE(result.hasValue()) << "subscribe should fail (publisher reconnected)";
    if (result.hasError()) {
      EXPECT_EQ(result.error().reasonPhrase, "publisher reconnected during subscribe");
    }
  });

  if (pub2Consumer) {
    pub2Consumer->publishDone(
        {RequestID(0), PublishDoneStatusCode::SESSION_CLOSED, 0, "test cleanup"}
    );
  }
  relay_->doPublishNamespaceDone(kTestNamespace, publisherSession2);
}

} // namespace moxygen::test
