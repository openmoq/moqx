/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Originally from github.com/facebookexperimental/moxygen.
 * See deps/moxygen/LICENSE for the original license terms.
 *
 * Copyright (c) OpenMOQ contributors.
 */

#include "MoqxRelayTestFixture.h"

namespace moxygen::test {

// Test: Verify allowed namespace prefix is set correctly
TEST_P(MoQRelayTest, AllowedNamespacePrefix) {
  // This just verifies the relay can be constructed with a namespace prefix
  // More detailed testing requires full session setup
  auto relay2 = std::make_shared<MoqxRelay>(config::CacheConfig{
      .maxCachedTracks = openmoq::moqx::kDefaultMaxCachedTracks,
      .maxCachedGroupsPerTrack = openmoq::moqx::kDefaultMaxCachedGroupsPerTrack,
  });
  relay2->setAllowedNamespacePrefix(kTestNamespace);
  EXPECT_NE(relay2, nullptr);
}

// Test: Publish a track through the relay
TEST_P(MoQRelayTest, PublishSuccess) {
  auto publisherSession = createMockSession();

  // Publish the namespace
  doPublishNamespace(publisherSession, kTestNamespace);

  // Publish the track
  doPublish(publisherSession, kTestTrackName);

  // Cleanup: remove the session from relay to avoid mock leak warning
  exec_->drive();
  removeSession(publisherSession);
}

// Namespace tree tests (Prune*, MixedContent*, ActiveChildCount, PublishKeepsNode)
// are in NamespaceTreeTest.cpp.
// MoQForwarder unit tests (draining, tombstoning, hard errors, etc.)
// are covered by moxygen's MoQForwarderTest.

// ============================================================
// Extensions Tests
// ============================================================

// Test: Extensions from publish are forwarded to subscribers via
// subscribeNamespace
TEST_P(MoQRelayTest, PublishExtensionsForwardedToSubscribers) {
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
    auto res = subscriberInterface()->publish(std::move(pub), createMockSubscriptionHandle());
    EXPECT_TRUE(res.hasValue());
    if (res.hasValue()) {
      getOrCreateMockState(publisherSession)->publishConsumers.push_back(res->consumer);
      co_withExecutor(static_cast<folly::DrivableExecutor*>(exec_.get()), std::move(res->reply))
          .start();
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

// Test: Extensions from publish are forwarded to late-joining subscribers
TEST_P(MoQRelayTest, PublishExtensionsForwardedToLateJoiners) {
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
    auto res = subscriberInterface()->publish(std::move(pub), createMockSubscriptionHandle());
    EXPECT_TRUE(res.hasValue());
    if (res.hasValue()) {
      getOrCreateMockState(publisherSession)->publishConsumers.push_back(res->consumer);
      co_withExecutor(static_cast<folly::DrivableExecutor*>(exec_.get()), std::move(res->reply))
          .start();
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
TEST_P(MoQRelayTest, PublisherReconnectWithOpenSubgroupNoSegfault) {
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

  exec_->drive();
  removeSession(publisherSession);
  removeSession(subscriberSession);
  removeSession(reconnectedSession);
}

// ============================================================
// Publish Replaces Subscribe Tests
// ============================================================

// Regression test: When a PUBLISH replaces a subscribe-path subscription, the
// old forwarder's subscribers must receive publishDone, and the new
// publish-path subscription must be fully functional (accepting data from the
// new publisher).
TEST_P(MoQRelayTest, PublishReplacesSubscribeDrainsOldAndServesNew) {
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
TEST_P(MoQRelayTest, PublishReconnectDuringSubscribeScopeGuardCrash) {
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
              auto res =
                  subscriberInterface()->publish(std::move(pub), createMockSubscriptionHandle());
              EXPECT_TRUE(res.hasValue()) << "publish in mock unexpectedly failed";
              if (res.hasValue()) {
                pub2Consumer = res->consumer;
                co_await std::move(res->reply);
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
        publisherInterface()->subscribe(std::move(sub), createMockConsumer()),
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
TEST_P(MoQRelayTest, PublishReconnectDuringSubscribeSuccessPathCrash) {
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
              auto res =
                  subscriberInterface()->publish(std::move(pub), createMockSubscriptionHandle());
              EXPECT_TRUE(res.hasValue()) << "publish in mock unexpectedly failed";
              if (res.hasValue()) {
                pub2Consumer = res->consumer;
                co_await std::move(res->reply);
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
        publisherInterface()->subscribe(std::move(sub), createMockConsumer()),
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

// Regression: after publishDone the namespace-tree node must be pruned when
// the track was the only remaining content.  (Was a bug before unpublishTrack
// gained a NodeMutationGuard; kept as a regression guard.)
TEST_P(MoQRelayTest, PublishDonePrunesNamespaceTreeNode) {
  auto publisher = createMockSession();

  doPublishNamespace(publisher, kTestNamespace);
  auto consumer = doPublish(publisher, kTestTrackName);

  // Verify the publish is visible in the tree
  verifyOnRelayExec([&] {
    auto state = relay_->findPublishState(kTestTrackName);
    EXPECT_TRUE(state.nodeExists);
    EXPECT_EQ(state.session, publisher);
  });

  // publishNamespaceDone — node stays alive because the track publish is still active
  withSessionContext(publisher, [&]() {
    getOrCreateMockState(publisher)->publishNamespaceHandles[0]->publishNamespaceDone();
    getOrCreateMockState(publisher)->publishNamespaceHandles.clear();
  });

  verifyOnRelayExec([&] {
    auto state = relay_->findPublishState(kTestTrackName);
    EXPECT_TRUE(state.nodeExists);
    EXPECT_EQ(state.session, publisher);
  });

  // End the track publish — node should now be pruned
  withSessionContext(publisher, [&]() {
    consumer->publishDone(
        {RequestID(0), PublishDoneStatusCode::SUBSCRIPTION_ENDED, 0, "publisher done"}
    );
  });

  verifyOnRelayExec([&] {
    auto state = relay_->findPublishState(kTestTrackName);
    EXPECT_EQ(state.session, nullptr);
    EXPECT_FALSE(state.nodeExists) << "Node persists after publish ended; pruning did not run";
  });

  exec_->drive();
  removeSession(publisher);
}

// Empty namespace: publishNamespace with an empty TrackNamespace must not crash.
TEST_P(MoQRelayTest, EmptyNamespacePublishNamespaceDone) {
  auto publisher = createMockSession();

  TrackNamespace emptyNs{{}};
  PublishNamespace ann;
  ann.trackNamespace = emptyNs;
  withSessionContext(publisher, [&]() {
    auto task = subscriberInterface()->publishNamespace(std::move(ann), nullptr);
    auto res = folly::coro::blockingWait(std::move(task), exec_.get());
    if (res.hasValue()) {
      getOrCreateMockState(publisher)->publishNamespaceHandles.push_back(res.value());
    }
  });

  removeSession(publisher);
}

} // namespace moxygen::test
