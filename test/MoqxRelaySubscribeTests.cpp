/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Copyright (c) OpenMOQ contributors.
 * Originally from github.com/facebookexperimental/moxygen.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MoqxRelayTestFixture.h"

#include <atomic>
#include <folly/synchronization/Baton.h>

namespace openmoq::moqx::test {

// Test: SUBSCRIBE with an empty namespace is rejected pre-draft-18.
TEST_P(MoQRelayTest, SubscribeEmptyNamespaceRejectedPreV18) {
  auto session = createMockSession();
  // Default session negotiates kVersionDraft16 (< 18)

  auto consumer = createMockConsumer();
  auto handle = subscribeToTrack(
      session,
      FullTrackName{TrackNamespace{{}}, "track1"},
      consumer,
      RequestID(0),
      /*addToState=*/false,
      SubscribeErrorCode::DOES_NOT_EXIST
  );
  EXPECT_EQ(handle, nullptr);

  removeSession(session);
}

// Test: SUBSCRIBE with an empty namespace is accepted on draft-18+.
TEST_P(MoQRelayTest, SubscribeEmptyNamespaceAllowedV18) {
  relay_->setAllowedNamespacePrefix(TrackNamespace{{}});
  FullTrackName emptyNsTrack{TrackNamespace{{}}, "track1"};

  auto publisherSession = createMockSession();
  ON_CALL(*publisherSession, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft18)));
  doPublish(publisherSession, emptyNsTrack);

  auto subSession = createMockSession();
  ON_CALL(*subSession, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft18)));
  auto consumer = createMockConsumer();
  auto handle = subscribeToTrack(subSession, emptyNsTrack, consumer, RequestID(0));
  ASSERT_NE(handle, nullptr);

  handle->unsubscribe();
  exec_->drive();
  removeSession(publisherSession);
  removeSession(subSession);
}

// Test: forwardChanged must not crash when called after the publisher has
// terminated (onPublishDone clears handle/upstream). We trigger forwardChanged
// via Subscriber::requestUpdate changing forward from true→false (1→0
// transition). The subscriber survives drain because it has an open subgroup.
TEST_P(MoQRelayTest, ForwardChangedAfterPublisherTermination) {
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
      .WillOnce([&sg](uint64_t, uint64_t, uint8_t, moxygen::BeginSubgroupOptions) {
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(sg);
      });
  auto subgroupRes = publishConsumer->beginSubgroup(0, 0, 0);
  ASSERT_TRUE(subgroupRes.hasValue());
  driveIfMultiThread(
  ); // flush beginSubgroup so relay subgroup forwarder is wired before publishDone

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
  driveIfMultiThread(); // flush pending lambdas (sg->reset, cleanup) before mocks are destroyed
}

// Bug: when a second subscriber with forward=true joins an existing PUBLISH-path
// subscription (causing a 0→1 forwarding transition), the relay fires REQUEST_UPDATE
// twice — once via forwardChanged() (which fires synchronously inside addSubscriber
// via addForwardingSubscriber) and once via the explicit block at the end of the
// subscribe() else-branch. Analogous to the subscribeNamespace bug fixed in this PR.
TEST_P(MoQRelayTest, Subscribe_SecondForwardingSubscriber_SingleRequestUpdate) {
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
          publisherInterface()->subscribe(std::move(sub), createMockConsumer()),
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
          publisherInterface()->subscribe(std::move(sub), createMockConsumer()),
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

// Safety net for the planned attachLocalForwarderToPrimary reshape (and the merge
// of the two primaryExec sorties): the first-subscriber path that issues an
// upstream SUBSCRIBE, installs the relay chain, and wires the local/primary
// forwarder must still deliver objects to the downstream subscriber in all three
// modes. Data-plane tests only cover the published-track path (doPublish); this
// covers announce-then-pull, where the relay subscribes upstream and the upstream
// is the data source.
TEST_P(MoQRelayTest, FirstSubscriberViaUpstreamSubscribeReceivesData) {
  auto publisherSession = createMockSession();
  auto subSession = createMockSession();

  // Publisher announces the namespace but does NOT publish, so the first
  // subscriber forces the relay to SUBSCRIBE upstream to pull the track.
  doPublishNamespace(publisherSession, kTestNamespace);

  // Capture the upstream consumer (the relay's writeback into the primary
  // forwarder) so the test can act as the upstream source and push data.
  std::shared_ptr<TrackConsumer> upstreamConsumer;
  SubscribeOk upstreamOk;
  upstreamOk.requestID = RequestID(1);
  upstreamOk.trackAlias = TrackAlias(1);
  upstreamOk.expires = std::chrono::milliseconds(0);
  upstreamOk.groupOrder = GroupOrder::OldestFirst;
  EXPECT_CALL(*publisherSession, subscribe(_, _))
      .WillOnce([&upstreamConsumer,
                 upstreamOk](const SubscribeRequest&, std::shared_ptr<TrackConsumer> consumer) {
        upstreamConsumer = std::move(consumer);
        auto handle = std::make_shared<NiceMock<MockSubscriptionHandle>>(upstreamOk);
        return folly::coro::makeTask<Publisher::SubscribeResult>(
            folly::Expected<std::shared_ptr<SubscriptionHandle>, SubscribeError>(handle)
        );
      });

  // First subscriber: drives the upstream subscribe + relay-chain wiring.
  auto consumer = createMockConsumer();
  auto sg = createMockSubgroupConsumer();
  std::atomic<bool> gotSubgroup{false};
  EXPECT_CALL(*consumer, beginSubgroup(0, 0, _, _))
      .WillOnce([&sg, &gotSubgroup](uint64_t, uint64_t, uint8_t, moxygen::BeginSubgroupOptions) {
        gotSubgroup.store(true);
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(sg);
      });

  auto handle = subscribeToTrack(subSession, kTestTrackName, consumer, RequestID(0));
  ASSERT_NE(handle, nullptr);
  ASSERT_NE(upstreamConsumer, nullptr) << "relay should have issued an upstream subscribe";

  // Act as the upstream source: push a subgroup. It must traverse the relay chain
  // (and, in LocalForwarderMT, the primary->localFwd channel sub) to the downstream.
  auto sgRes = upstreamConsumer->beginSubgroup(0, 0, 0);
  ASSERT_TRUE(sgRes.hasValue());
  EXPECT_TRUE(sgRes.value()->endOfSubgroup().hasValue());

  EXPECT_TRUE(driveUntil([&] { return gotSubgroup.load(); }))
      << "downstream subscriber should receive the upstream subgroup in mode "
      << static_cast<int>(relayMode());

  removeSession(publisherSession);
  removeSession(subSession);
  driveIfMultiThread();
}

// Regression: a second subscriber that arrives while a first subscriber's upstream
// SUBSCRIBE is still in flight must observe the upstream-seeded largest. In
// LocalForwarderMT the second takes the acquireLocalForwarder isNew=false fast path
// and, without a readiness gate, reads the not-yet-seeded localFwd largest (empty).
// ST/MT wait on the registry promise and were already correct, so this passes in
// every mode and regresses only LF.
TEST_P(MoQRelayTest, SubsequentSubscriberWaitsForUpstreamLargestSeeding) {
  auto publisherSession = createMockSession();
  auto subSession1 = createMockSession();
  auto subSession2 = createMockSession();

  doPublishNamespace(publisherSession, kTestNamespace);

  const AbsoluteLocation kLargest{3, 0};
  SubscribeOk upstreamOk;
  upstreamOk.requestID = RequestID(1);
  upstreamOk.trackAlias = TrackAlias(1);
  upstreamOk.expires = std::chrono::milliseconds(0);
  upstreamOk.groupOrder = GroupOrder::OldestFirst;
  upstreamOk.largest = kLargest;

  // Hold the upstream SUBSCRIBE in flight so the second subscriber races in while the
  // first subscriber's largest is still unseeded.
  folly::coro::Baton upstreamGate;
  std::atomic<bool> upstreamSubscribeCalled{false};
  std::shared_ptr<TrackConsumer> upstreamConsumer;
  EXPECT_CALL(*publisherSession, subscribe(_, _))
      .WillOnce(
          [&](const SubscribeRequest&, std::shared_ptr<TrackConsumer> consumer
          ) -> folly::coro::Task<Publisher::SubscribeResult> {
            upstreamConsumer = std::move(consumer);
            upstreamSubscribeCalled.store(true);
            co_await upstreamGate;
            auto handle = std::make_shared<NiceMock<MockSubscriptionHandle>>(upstreamOk);
            co_return folly::Expected<std::shared_ptr<SubscriptionHandle>, SubscribeError>(handle);
          }
      );

  // driveUntil caps SingleThread at one loopOnce; pump explicitly so the synchronous
  // cascades complete in every mode.
  auto pump = [&](auto pred) {
    for (int i = 0; i < 1000 && !pred(); ++i) {
      exec_->drive();
    }
    return pred();
  };
  auto launchSubscribe = [&](std::shared_ptr<MoQSession> session,
                             std::shared_ptr<TrackConsumer> consumer,
                             RequestID requestID,
                             std::shared_ptr<std::optional<Publisher::SubscribeResult>> out) {
    withSessionContext(session, [&]() {
      SubscribeRequest sub;
      sub.fullTrackName = kTestTrackName;
      sub.requestID = requestID;
      sub.locType = LocationType::LargestObject;
      auto task = publisherInterface()->subscribe(std::move(sub), std::move(consumer));
      co_withExecutor(
          static_cast<folly::DrivableExecutor*>(exec_.get()),
          folly::coro::co_invoke([t = std::move(task), out]() mutable -> folly::coro::Task<void> {
            *out = co_await std::move(t);
          })
      ).start();
    });
  };

  // First subscriber: creates the shared localFwd in acquireLocalForwarder, then
  // suspends inside the gated upstream SUBSCRIBE.
  auto firstResult = std::make_shared<std::optional<Publisher::SubscribeResult>>();
  launchSubscribe(subSession1, createMockConsumer(), RequestID(0), firstResult);
  ASSERT_TRUE(pump([&] { return upstreamSubscribeCalled.load(); }))
      << "relay should issue an upstream subscribe and suspend in it";

  // Second subscriber, while the first's seeding is still pending. Drive it to its
  // steady state (LF without the gate attaches immediately; ST/MT/LF-with-gate wait)
  // before releasing the upstream OK, so a captured result reflects the race.
  auto secondResult = std::make_shared<std::optional<Publisher::SubscribeResult>>();
  launchSubscribe(subSession2, createMockConsumer(), RequestID(2), secondResult);
  for (int i = 0; i < 200; ++i) {
    exec_->drive();
  }

  upstreamGate.post();
  ASSERT_TRUE(pump([&] { return firstResult->has_value() && secondResult->has_value(); }));

  ASSERT_TRUE(firstResult->value().hasValue());
  EXPECT_EQ(firstResult->value().value()->subscribeOk().largest, kLargest);
  ASSERT_TRUE(secondResult->value().hasValue());
  EXPECT_EQ(secondResult->value().value()->subscribeOk().largest, kLargest)
      << "subsequent subscriber must observe the upstream-seeded largest, not a "
         "pre-seeding value";

  // Track the handles so cleanupMockSession tears down the subscriptions (else the
  // held consumers/sessions leak as unverified mocks at exit).
  getOrCreateMockState(subSession1)->subscribeHandles.push_back(firstResult->value().value());
  getOrCreateMockState(subSession2)->subscribeHandles.push_back(secondResult->value().value());

  removeSession(publisherSession);
  removeSession(subSession1);
  removeSession(subSession2);
  driveIfMultiThread();
}

// Regression: when the first subscriber's upstream SUBSCRIBE *fails*, a second
// subscriber that raced in behind it must fail too. In LocalForwarderMT the second took
// the isNew=false path and awaited the ready gate, which the first fulfills with
// setValue() on every exit — including failure. The second then attached to a localFwd
// that setup had already removed from the registry and that has no upstream, so it got a
// SUBSCRIBE_OK for a track that can never deliver an object.
TEST_P(MoQRelayTest, SubsequentSubscriberFailsWhenUpstreamSubscribeFails) {
  auto publisherSession = createMockSession();
  auto subSession1 = createMockSession();
  auto subSession2 = createMockSession();

  doPublishNamespace(publisherSession, kTestNamespace);

  // Hold the upstream SUBSCRIBE in flight so the second subscriber races in, then reject
  // it — the first subscriber's setup fails after the second is already waiting.
  folly::coro::Baton upstreamGate;
  std::atomic<bool> upstreamSubscribeCalled{false};
  EXPECT_CALL(*publisherSession, subscribe(_, _))
      .WillOnce(
          [&](const SubscribeRequest&,
              std::shared_ptr<TrackConsumer>) -> folly::coro::Task<Publisher::SubscribeResult> {
            upstreamSubscribeCalled.store(true);
            co_await upstreamGate;
            co_return folly::makeUnexpected(SubscribeError{
                RequestID(0),
                SubscribeErrorCode::INTERNAL_ERROR,
                "upstream rejected"
            });
          }
      );

  auto pump = [&](auto pred) {
    for (int i = 0; i < 1000 && !pred(); ++i) {
      exec_->drive();
    }
    return pred();
  };
  auto launchSubscribe = [&](std::shared_ptr<MoQSession> session,
                             std::shared_ptr<TrackConsumer> consumer,
                             RequestID requestID,
                             std::shared_ptr<std::optional<Publisher::SubscribeResult>> out) {
    withSessionContext(session, [&]() {
      SubscribeRequest sub;
      sub.fullTrackName = kTestTrackName;
      sub.requestID = requestID;
      sub.locType = LocationType::LargestObject;
      auto task = publisherInterface()->subscribe(std::move(sub), std::move(consumer));
      // ST/MT signal first-subscriber failure by throwing out of the SubsequentSubscriber
      // task; normalize to an error result so both shapes are comparable.
      co_withExecutor(
          static_cast<folly::DrivableExecutor*>(exec_.get()),
          folly::coro::co_invoke([t = std::move(task), out]() mutable -> folly::coro::Task<void> {
            try {
              *out = co_await std::move(t);
            } catch (const std::exception& e) {
              *out = folly::makeUnexpected(
                  SubscribeError{RequestID(0), SubscribeErrorCode::INTERNAL_ERROR, e.what()}
              );
            }
          })
      ).start();
    });
  };

  auto firstResult = std::make_shared<std::optional<Publisher::SubscribeResult>>();
  launchSubscribe(subSession1, createMockConsumer(), RequestID(0), firstResult);
  ASSERT_TRUE(pump([&] { return upstreamSubscribeCalled.load(); }))
      << "relay should issue an upstream subscribe and suspend in it";

  // Second subscriber, parked on the ready gate while the first is still in flight.
  auto secondResult = std::make_shared<std::optional<Publisher::SubscribeResult>>();
  launchSubscribe(subSession2, createMockConsumer(), RequestID(2), secondResult);
  for (int i = 0; i < 200; ++i) {
    exec_->drive();
  }

  upstreamGate.post();
  ASSERT_TRUE(pump([&] { return firstResult->has_value() && secondResult->has_value(); }));

  EXPECT_FALSE(firstResult->value().hasValue()) << "upstream rejected the subscribe";
  EXPECT_FALSE(secondResult->value().hasValue())
      << "a subscriber released by a FAILED setup must get an error, not a SUBSCRIBE_OK "
         "for a forwarder with no upstream";

  if (firstResult->value().hasValue()) {
    getOrCreateMockState(subSession1)->subscribeHandles.push_back(firstResult->value().value());
  }
  if (secondResult->value().hasValue()) {
    getOrCreateMockState(subSession2)->subscribeHandles.push_back(secondResult->value().value());
  }

  removeSession(publisherSession);
  removeSession(subSession1);
  removeSession(subSession2);
  driveIfMultiThread();
}

// The publisher-exec slot claim arms a gate in the PUBLISHER thread's
// tlForwarders_, which the subscriber-side signalReady guard cannot reach. If the upstream
// subscribe throws rather than returning an error, the openReady/failReady pair after it is
// skipped and any subscriber parked on that gate never wakes.
TEST_P(MoQRelayTest, UpstreamSubscribeThrowDoesNotStrandGate) {
  if (relayMode() != RelayMode::LocalForwarderMT) {
    GTEST_SKIP() << "only LF has a per-thread registry to strand";
  }

  auto& pubAux = makeAuxExec("pub-iothread");
  auto* pubEvb = pubAux.evb;
  auto pubExec = pubAux.exec;

  auto makeSessionOnPubThread = [&] {
    auto session = std::make_shared<NiceMock<MockMoQSession>>(pubExec);
    ON_CALL(*session, getNegotiatedVersion())
        .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft16)));
    getOrCreateMockState(session);
    return session;
  };
  auto publisherSession = makeSessionOnPubThread();
  auto subSessionOnPubThread = makeSessionOnPubThread();
  auto subSession1 = createMockSession();

  doPublishNamespace(publisherSession, kTestNamespace);

  folly::coro::Baton upstreamGate;
  std::atomic<bool> upstreamSubscribeCalled{false};
  EXPECT_CALL(*publisherSession, subscribe(_, _))
      .WillOnce(
          [&](const SubscribeRequest&,
              std::shared_ptr<TrackConsumer>) -> folly::coro::Task<Publisher::SubscribeResult> {
            upstreamSubscribeCalled.store(true);
            co_await upstreamGate;
            throw std::runtime_error("upstream session blew up");
          }
      );
  auto releaseGate = folly::makeGuard([&]() noexcept { upstreamGate.post(); });

  auto pump = [&](auto pred) {
    for (int i = 0; i < 1000 && !pred(); ++i) {
      exec_->drive();
      pubEvb->runInEventBaseThreadAndWait([] {});
    }
    return pred();
  };

  std::atomic<bool> firstDone{false};
  withSessionContext(subSession1, [&]() {
    SubscribeRequest sub;
    sub.fullTrackName = kTestTrackName;
    sub.requestID = RequestID(0);
    sub.locType = LocationType::LargestObject;
    auto task = publisherInterface()->subscribe(std::move(sub), createMockConsumer());
    co_withExecutor(
        static_cast<folly::DrivableExecutor*>(exec_.get()),
        folly::coro::co_invoke(
            [t = std::move(task), &firstDone]() mutable -> folly::coro::Task<void> {
              auto r = co_await folly::coro::co_awaitTry(std::move(t));
              firstDone.store(true);
            }
        )
    ).start();
  });
  ASSERT_TRUE(pump([&] { return upstreamSubscribeCalled.load(); }));

  auto secondConsumer = createMockConsumer();
  folly::Baton<> secondDone;
  pubEvb->runInEventBaseThreadAndWait([&]() {
    withSessionContext(subSessionOnPubThread, [&]() {
      SubscribeRequest sub;
      sub.fullTrackName = kTestTrackName;
      sub.requestID = RequestID(2);
      sub.locType = LocationType::LargestObject;
      auto task = publisherInterface()->subscribe(std::move(sub), secondConsumer);
      co_withExecutor(
          folly::getKeepAliveToken(pubEvb),
          folly::coro::co_invoke(
              [t = std::move(task), &secondDone]() mutable -> folly::coro::Task<void> {
                auto r = co_await folly::coro::co_awaitTry(std::move(t));
                secondDone.post();
              }
          )
      ).start();
    });
  });
  pubEvb->runInEventBaseThreadAndWait([]() {});

  releaseGate.dismiss();
  upstreamGate.post();
  ASSERT_TRUE(pump([&] { return firstDone.load(); })) << "first subscribe never unwound";
  EXPECT_TRUE(pump([&] { return secondDone.try_wait_for(std::chrono::milliseconds(0)); }))
      << "subscriber parked on the publisher-thread ready gate never woke";

  removeSession(publisherSession);
  removeSession(subSession1);
  removeSession(subSessionOnPubThread);
  for (int i = 0; i < 4; ++i) {
    driveIfMultiThread();
    pubEvb->runInEventBaseThreadAndWait([]() {});
  }
}
// Cross-thread sibling of SubsequentSubscriberWaitsForUpstreamLargestSeeding: sub2 on its own
// OS thread is gated by the registry awaitSubsequent future (not the per-thread readiness gate).
TEST_P(MoQRelayTest, CrossThreadSubsequentSubscriberSeedingRace) {
  auto publisherSession = createMockSession(); // upstream/publisher on exec_
  auto subSession1 = createMockSession();      // first subscriber on exec_

  // Second subscriber on a dedicated OS thread => distinct thread-local tlForwarders_.
  auto& subAux = makeAuxExec("sub2-thread");
  auto subExec = subAux.exec;
  auto subSession2 = std::make_shared<NiceMock<MockMoQSession>>(subExec);
  ON_CALL(*subSession2, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft16)));
  getOrCreateMockState(subSession2);

  doPublishNamespace(publisherSession, kTestNamespace);

  const AbsoluteLocation kLargest{3, 0};
  SubscribeOk upstreamOk;
  upstreamOk.requestID = RequestID(1);
  upstreamOk.trackAlias = TrackAlias(1);
  upstreamOk.expires = std::chrono::milliseconds(0);
  upstreamOk.groupOrder = GroupOrder::OldestFirst;
  upstreamOk.largest = kLargest;

  // Hold the upstream SUBSCRIBE in flight so sub2 races in while sub1's largest is unseeded.
  folly::coro::Baton upstreamGate;
  std::atomic<bool> upstreamSubscribeCalled{false};
  EXPECT_CALL(*publisherSession, subscribe(_, _))
      .WillOnce(
          [&](const SubscribeRequest&,
              std::shared_ptr<TrackConsumer>) -> folly::coro::Task<Publisher::SubscribeResult> {
            upstreamSubscribeCalled.store(true);
            co_await upstreamGate;
            auto handle = std::make_shared<NiceMock<MockSubscriptionHandle>>(upstreamOk);
            co_return folly::Expected<std::shared_ptr<SubscriptionHandle>, SubscribeError>(handle);
          }
      );

  // The cross-exec sortie runs on exec_; drive it and flush the sub2 thread each iteration.
  auto pumpExec = [&](auto pred) {
    for (int i = 0; i < 2000 && !pred(); ++i) {
      exec_->drive();
      subAux.evb->runInEventBaseThreadAndWait([] {});
    }
    return pred();
  };

  auto launchSubscribe = [&](std::shared_ptr<MoQSession> session,
                             folly::Executor* startExec,
                             RequestID requestID,
                             std::shared_ptr<std::optional<Publisher::SubscribeResult>> out,
                             std::atomic<bool>* done) {
    withSessionContext(session, [&]() {
      SubscribeRequest sub;
      sub.fullTrackName = kTestTrackName;
      sub.requestID = requestID;
      sub.locType = LocationType::LargestObject;
      auto task = publisherInterface()->subscribe(std::move(sub), createMockConsumer());
      co_withExecutor(
          startExec,
          folly::coro::co_invoke(
              [t = std::move(task), out, done]() mutable -> folly::coro::Task<void> {
                *out = co_await std::move(t);
                if (done) {
                  done->store(true);
                }
              }
          )
      ).start();
    });
  };

  // First subscriber on exec_: becomes firstSetup, suspends in the gated upstream SUBSCRIBE.
  auto firstResult = std::make_shared<std::optional<Publisher::SubscribeResult>>();
  launchSubscribe(
      subSession1,
      static_cast<folly::DrivableExecutor*>(exec_.get()),
      RequestID(0),
      firstResult,
      nullptr
  );
  ASSERT_TRUE(pumpExec([&] { return upstreamSubscribeCalled.load(); }))
      << "relay should issue an upstream subscribe and suspend in it";

  // Second subscriber on its own thread, while the first's seeding is still pending.
  std::atomic<bool> sub2Done{false};
  auto secondResult = std::make_shared<std::optional<Publisher::SubscribeResult>>();
  launchSubscribe(subSession2, subExec.get(), RequestID(2), secondResult, &sub2Done);

  // sub2 must stay blocked on awaitSubsequent until the upstream OK seeds largest; resolving
  // early would return an empty largest a client reads as a track restart.
  EXPECT_FALSE(pumpExec([&] { return sub2Done.load(); }))
      << "cross-thread subsequent subscriber resolved before the upstream OK seeded largest";

  upstreamGate.post();
  ASSERT_TRUE(pumpExec([&] { return firstResult->has_value() && sub2Done.load(); }));

  ASSERT_TRUE(firstResult->value().hasValue());
  EXPECT_EQ(firstResult->value().value()->subscribeOk().largest, kLargest);
  ASSERT_TRUE(secondResult->value().hasValue());
  // Post-OK the established largest must hold regardless of when sub2 resolved.
  EXPECT_EQ(secondResult->value().value()->subscribeOk().largest, kLargest);

  getOrCreateMockState(subSession1)->subscribeHandles.push_back(firstResult->value().value());
  getOrCreateMockState(subSession2)->subscribeHandles.push_back(secondResult->value().value());

  removeSession(publisherSession);
  removeSession(subSession1);
  removeSession(subSession2);
  driveIfMultiThread();
  subAux.evb->runInEventBaseThreadAndWait([] {});
}

// Regression: a PUBLISH that fans out to a thread where a SUBSCRIBE for the same track is
// parked mid-setup must not join that thread's registry entry while it is Pending. The
// entry stays Pending for the whole upstream SUBSCRIBE round trip, and acquireLocalForwarder
// XCHECKed on Pending instead of handling it, aborting the process. The parked subscribe
// owns the track, so the fanout drops rather than publishing over it.
TEST_P(MoQRelayTest, PublishFanoutDuringParkedSubscribeSetup) {
  if (relayMode() != RelayMode::LocalForwarderMT) {
    GTEST_SKIP() << "only LF has a per-thread registry that can be mid-setup";
  }

  // Publisher on its own iothread, so its installPublisherForwarder lands in that thread's
  // registry and leaves the subscriber thread's entry Pending for the fanout to find.
  auto& pubAux = makeAuxExec("pub-iothread");
  auto* pubEvb = pubAux.evb;
  auto pubExec = pubAux.exec;
  auto publisherSession = std::make_shared<NiceMock<MockMoQSession>>(pubExec);
  ON_CALL(*publisherSession, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft16)));
  getOrCreateMockState(publisherSession);

  // One session on exec_ holding both the SUBSCRIBE_NAMESPACE that makes it a fanout
  // target and the SUBSCRIBE that parks.
  auto subSession = createMockSession();
  setupPublishSucceeds(subSession);
  doPublishNamespace(publisherSession, kTestNamespace);
  doSubscribeNamespace(subSession, kTestNamespace);

  // The parked subscribe owns the track; the fanout must drop rather than publish over it.
  EXPECT_CALL(*subSession, publish(_, _)).Times(0);

  SubscribeOk upstreamOk;
  upstreamOk.requestID = RequestID(1);
  upstreamOk.trackAlias = TrackAlias(1);
  upstreamOk.expires = std::chrono::milliseconds(0);
  upstreamOk.groupOrder = GroupOrder::OldestFirst;

  // Hold the upstream SUBSCRIBE so the subscribe keeps its registry claim open.
  folly::coro::Baton upstreamGate;
  std::atomic<bool> upstreamSubscribeCalled{false};
  EXPECT_CALL(*publisherSession, subscribe(_, _))
      .WillOnce(
          [&](const SubscribeRequest&,
              std::shared_ptr<TrackConsumer>) -> folly::coro::Task<Publisher::SubscribeResult> {
            upstreamSubscribeCalled.store(true);
            co_await upstreamGate;
            auto handle = std::make_shared<NiceMock<MockSubscriptionHandle>>(upstreamOk);
            co_return folly::Expected<std::shared_ptr<SubscriptionHandle>, SubscribeError>(handle);
          }
      );
  auto releaseGate = folly::makeGuard([&]() noexcept { upstreamGate.post(); });

  auto pump = [&](auto pred) {
    for (int i = 0; i < 1000 && !pred(); ++i) {
      exec_->drive();
      pubEvb->runInEventBaseThreadAndWait([] {});
    }
    return pred();
  };

  auto subConsumer = createMockConsumer();
  auto subResult = std::make_shared<std::optional<Publisher::SubscribeResult>>();
  std::atomic<bool> subscribeDone{false};
  withSessionContext(subSession, [&]() {
    SubscribeRequest sub;
    sub.fullTrackName = kTestTrackName;
    sub.requestID = RequestID(0);
    sub.locType = LocationType::LargestObject;
    auto task = publisherInterface()->subscribe(std::move(sub), subConsumer);
    co_withExecutor(
        static_cast<folly::DrivableExecutor*>(exec_.get()),
        folly::coro::co_invoke(
            [t = std::move(task), subResult, &subscribeDone]() mutable -> folly::coro::Task<void> {
              *subResult = co_await std::move(t);
              subscribeDone.store(true);
            }
        )
    ).start();
  });
  ASSERT_TRUE(pump([&] { return upstreamSubscribeCalled.load(); }))
      << "subscribe should park in the gated upstream SUBSCRIBE";

  // The racing PUBLISH, issued from the publisher's own thread.
  std::shared_ptr<TrackConsumer> pubConsumer;
  pubEvb->runInEventBaseThreadAndWait([&]() {
    withSessionContext(publisherSession, [&]() {
      PublishRequest pub;
      pub.fullTrackName = kTestTrackName;
      auto res = subscriberInterface()->publish(std::move(pub), createMockSubscriptionHandle());
      EXPECT_TRUE(res.hasValue());
      if (res.hasValue()) {
        pubConsumer = res->consumer;
        co_withExecutor(folly::getKeepAliveToken(pubEvb), std::move(res->reply)).start();
      }
    });
  });
  ASSERT_NE(pubConsumer, nullptr);

  // Run the fanout to completion while the entry is still Pending — this is where the
  // XCHECK fired — before the gate opens and the entry goes Ready.
  for (int i = 0; i < 50; ++i) {
    exec_->drive();
    pubEvb->runInEventBaseThreadAndWait([] {});
  }

  releaseGate.dismiss();
  upstreamGate.post();
  ASSERT_TRUE(pump([&] { return subscribeDone.load(); })) << "parked subscribe never unwound";
  // The publish replaced the registry entry the subscribe was still setting up, so the
  // subscribe loses. Both racers resolving is what matters; neither aborts the process.
  EXPECT_FALSE(subResult->value().hasValue());

  // The track is left usable: a fresh subscribe attaches to the publish and delivers.
  auto sg = createMockSubgroupConsumer();
  std::atomic<bool> gotSubgroup{false};
  auto retryConsumer = createMockConsumer();
  EXPECT_CALL(*retryConsumer, beginSubgroup(0, 0, _, _))
      .WillOnce([&sg, &gotSubgroup](uint64_t, uint64_t, uint8_t, moxygen::BeginSubgroupOptions) {
        gotSubgroup.store(true);
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(sg);
      });
  ASSERT_NE(subscribeToTrack(subSession, kTestTrackName, retryConsumer, RequestID(1)), nullptr);
  pubEvb->runInEventBaseThreadAndWait([&]() {
    auto sgRes = pubConsumer->beginSubgroup(0, 0, 0);
    EXPECT_TRUE(sgRes.hasValue());
    if (sgRes.hasValue()) {
      EXPECT_TRUE(sgRes.value()->endOfSubgroup().hasValue());
    }
  });
  EXPECT_TRUE(pump([&] { return gotSubgroup.load(); }))
      << "track unusable after the publish/subscribe race";

  removeSession(publisherSession);
  removeSession(subSession);
  for (int i = 0; i < 4; ++i) {
    driveIfMultiThread();
    pubEvb->runInEventBaseThreadAndWait([] {});
  }
}

// A peer UNSUBSCRIBE reaches the forwarder on the session's io thread: MoQSession
// keeps the handle the relay published with and calls unsubscribe() on it directly,
// with no relay-exec hop. If onEmpty then runs the relay callback inline, registry_
// is erased off relayExec_ while the relay thread erases the same track, corrupting
// the F14 table (folly SafeAssert "clearTag: tags_[index] != 0"). Parking relayEvb_
// across the trigger keeps the assertion from being a race of its own.
TEST_P(MoQRelayTest, PeerUnsubscribeDefersOnEmptyToRelayExec) {
  if (relayMode() != RelayMode::MultiThread) {
    GTEST_SKIP() << "chain-forwarder callback wiring is RelayExec-only";
  }

  auto publisherSession = createMockSession();
  auto subSession = createMockSession();
  setupPublishSucceeds(subSession);

  doPublishNamespace(publisherSession, kTestNamespace);
  doSubscribeNamespace(subSession, kTestNamespace);
  auto publishHandle = makePublishHandle();
  ASSERT_NE(doPublishWithHandle(publisherSession, kTestTrackName, publishHandle), nullptr);
  driveIfMultiThread();
  ASSERT_NE(peerHandle(subSession), nullptr) << "relay should have published to the subscriber";
  // hasSubscribeOk() is non-virtual, so a wrapper that does not copy it silently
  // stops MoQSession seeding largest_ from the forwarder.
  EXPECT_TRUE(peerHandle(subSession)->hasSubscribeOk());

  std::atomic<bool> upstreamTornDown{false};
  EXPECT_CALL(*publishHandle, unsubscribe()).WillRepeatedly([&upstreamTornDown] {
    upstreamTornDown = true;
  });
  EXPECT_CALL(*publishHandle, requestUpdateCalled(_))
      .WillRepeatedly([&upstreamTornDown](RequestUpdate update) {
        if (update.forward && !*update.forward) {
          upstreamTornDown = true;
        }
      });

  // Shared, not stack: on a park timeout the test body returns while the lambda
  // is still queued, and it would wait on a destroyed baton.
  auto parked = std::make_shared<folly::Baton<>>();
  auto release = std::make_shared<folly::Baton<>>();
  relayEvb_->add([parked, release] {
    parked->post();
    release->wait();
  });
  ASSERT_TRUE(parked->try_wait_for(std::chrono::seconds(5)));

  peerUnsubscribe(subSession);
  exec_->driveSessionExecOnly();

  EXPECT_FALSE(upstreamTornDown.load())
      << "onEmpty reached MoqxRelay on the session executor instead of relayExec_";

  release->post();
  EXPECT_TRUE(driveUntil([&upstreamTornDown] { return upstreamTornDown.load(); }));

  removeSession(publisherSession);
  removeSession(subSession);
  driveIfMultiThread();
}
// Regression: a SUBSCRIBE that wins the first-subscriber race on one iothread must not be
// failed because a different subscriber on the publisher's iothread already claimed that
// thread's local-forwarder slot.
//
// The gate below forces this order:
//   1. subOnPubThread joins the publisher thread's registry and holds a pending claim on
//      the slot the publisher forwarder wants.
//   2. sub1, on another iothread, reaches relayExec first, so it is the FirstSubscriber and
//      hops to publisherExec to install the publisher forwarder.
//   3. The hop saw an occupant and failed the SUBSCRIBE, which erased the registry entry
//      and threw at everyone parked on it. subOnPubThread died too, the next arrival became
//      FirstSubscriber, and the cycle repeated.
TEST_P(MoQRelayTest, FirstSetupOverPendingClaimOnPublisherThread) {
  if (relayMode() != RelayMode::LocalForwarderMT) {
    GTEST_SKIP() << "only LF has a per-thread registry slot to contend for";
  }

  // Publisher on its own iothread: publisherExec is the thread whose registry slot
  // the first-setup hop inspects.
  auto& pubAux = makeAuxExec("pub-iothread");
  auto* pubEvb = pubAux.evb;
  auto publisherSession = createMockSessionOn(pubAux.exec);
  auto subSessionOnPubThread = createMockSessionOn(pubAux.exec);
  auto subSession1 = createMockSession(); // on exec_, a different iothread

  doPublishNamespace(publisherSession, kTestNamespace);

  SubscribeOk upstreamOk;
  upstreamOk.requestID = RequestID(1);
  upstreamOk.trackAlias = TrackAlias(1);
  upstreamOk.expires = std::chrono::milliseconds(0);
  upstreamOk.groupOrder = GroupOrder::OldestFirst;

  // The upstream SUBSCRIBE is built from whichever subscribe won FirstSubscriber, and it
  // carries this tag through, so the assertion below can name the winner.
  constexpr uint64_t kSub1Tag = 4242;
  std::shared_ptr<TrackConsumer> upstreamConsumer;
  std::atomic<int> upstreamSubscribes{0};
  std::atomic<uint64_t> upstreamTag{0};
  // Keeps the publisher forwarder pending for a whole upstream round trip, like production.
  // The displaced claim has to sit through that window.
  folly::coro::Baton upstreamGate;
  // The action is a plain lambda, not a coroutine: a coroutine body does not run until the
  // Task is awaited, and `req` is gmock's copy, which is gone by then. Read it here and let
  // the returned Task do the waiting.
  EXPECT_CALL(*publisherSession, subscribe(_, _))
      .WillOnce([&](const SubscribeRequest& req, std::shared_ptr<TrackConsumer> consumer) {
        upstreamConsumer = std::move(consumer);
        upstreamTag.store(
            getFirstIntParam(req.params, TrackRequestParamKey::DELIVERY_TIMEOUT).value_or(0)
        );
        upstreamSubscribes.fetch_add(1);
        return folly::coro::co_invoke(
            [&upstreamGate, upstreamOk]() -> folly::coro::Task<Publisher::SubscribeResult> {
              co_await upstreamGate;
              auto handle = std::make_shared<NiceMock<MockSubscriptionHandle>>(upstreamOk);
              co_return folly::Expected<std::shared_ptr<SubscriptionHandle>, SubscribeError>(handle
              );
            }
        );
      });
  auto releaseUpstream = folly::makeGuard([&]() noexcept { upstreamGate.post(); });

  auto consumer1 = createMockConsumer();
  auto consumerOnPubThread = createMockConsumer();
  std::atomic<bool> sub1GotData{false};
  std::atomic<bool> pubThreadSubGotData{false};
  auto sg1 = createMockSubgroupConsumer();
  auto sgPub = createMockSubgroupConsumer();
  EXPECT_CALL(*consumer1, beginSubgroup(0, 0, _, _))
      .WillOnce([&](uint64_t, uint64_t, uint8_t, moxygen::BeginSubgroupOptions) {
        sub1GotData.store(true);
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(sg1);
      });
  EXPECT_CALL(*consumerOnPubThread, beginSubgroup(0, 0, _, _))
      .WillOnce([&](uint64_t, uint64_t, uint8_t, moxygen::BeginSubgroupOptions) {
        pubThreadSubGotData.store(true);
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(sgPub);
      });

  // Hold relayExec so both subscribes queue their relay phase behind it, in a known
  // order, while each has already claimed its own thread's registry slot.
  auto relayGate = parkRelayExec();

  // sub1 claims exec_'s slot and queues its relay phase first, so it wins FirstSubscriber.
  auto sub1Req = makeSubscribeRequest(RequestID(0));
  sub1Req.params.insertParam(
      Parameter(folly::to_underlying(TrackRequestParamKey::DELIVERY_TIMEOUT), kSub1Tag)
  );
  auto sub1 = startSubscribe(sessionExec(), subSession1, std::move(sub1Req), consumer1);
  // driveSessionExecOnly, not drive(): drive() rendezvouses with the parked relayEvb_.
  for (int i = 0; i < 8; ++i) {
    exec_->driveSessionExecOnly();
  }

  // Now the publisher thread's own subscriber claims that thread's slot. Its relay
  // phase queues behind sub1's, so it becomes a subsequent subscriber while its
  // Pending claim still occupies the slot sub1 is about to install into.
  auto pubThreadSub = launchSubscribeOn(
      pubEvb,
      subSessionOnPubThread,
      makeSubscribeRequest(RequestID(2)),
      consumerOnPubThread
  );

  relayGate.release();
  ASSERT_TRUE(driveUntil([&] { return upstreamSubscribes.load() > 0; }))
      << "the first-setup subscribe should reach the upstream SUBSCRIBE";
  ASSERT_EQ(upstreamTag.load(), kSub1Tag)
      << "sub1 must win FirstSubscriber, or the slot it installs into holds its own claim "
         "and this stops covering a foreign one";
  releaseUpstream.dismiss();
  upstreamGate.post();
  ASSERT_TRUE(driveUntil([&] { return sub1.ready() && pubThreadSub.ready(); }))
      << "every subscribe should unwind";

  auto handle1 = sub1.handle();
  auto handleOnPubThread = pubThreadSub.handle();
  ASSERT_NE(handle1, nullptr);
  ASSERT_NE(handleOnPubThread, nullptr);
  getOrCreateMockState(subSession1)->subscribeHandles.push_back(handle1);
  getOrCreateMockState(subSessionOnPubThread)->subscribeHandles.push_back(handleOnPubThread);

  // Each must be wired to the live forwarder, not merely holding a SUBSCRIBE_OK.
  ASSERT_NE(upstreamConsumer, nullptr);
  pubEvb->runInEventBaseThreadAndWait([&]() {
    auto sgRes = upstreamConsumer->beginSubgroup(0, 0, 0);
    ASSERT_TRUE(sgRes.hasValue());
    EXPECT_TRUE(sgRes.value()->endOfSubgroup().hasValue());
  });
  EXPECT_TRUE(driveUntil([&] { return sub1GotData.load() && pubThreadSubGotData.load(); }))
      << "sub1=" << sub1GotData.load() << " pubThreadSub=" << pubThreadSubGotData.load();

  // The pub-thread sessions' handles unsubscribe from forwarders that thread owns, so
  // tear them down there rather than mutating those forwarders from the test thread.
  pubEvb->runInEventBaseThreadAndWait([&]() {
    removeSession(publisherSession);
    removeSession(subSessionOnPubThread);
  });
  removeSession(subSession1);
  drainExecs(4);
}

// A wake and the waiter's resumption are separate turns, so other work on that thread runs
// in between. Two PUBLISHes for one track land in that window: the first releases the
// waiter, the second takes the slot. The waiter has to end up on the forwarder that holds
// the track when it resumes, not the one that released it.
TEST_P(MoQRelayTest, ParkedSubscribeLandsOnTheLiveForwarderAfterARepublish) {
  if (relayMode() != RelayMode::LocalForwarderMT) {
    GTEST_SKIP() << "only LF has a per-thread registry slot to contend for";
  }

  auto& pubAux = makeAuxExec("pub-iothread");
  auto* pubEvb = pubAux.evb;
  auto firstPublisher = createMockSessionOn(pubAux.exec);
  auto secondPublisher = createMockSessionOn(pubAux.exec);
  // Holds the claim the waiter parks on, then loses the slot to the first PUBLISH.
  auto claimingSub = createMockSessionOn(pubAux.exec);
  auto parkedSub = createMockSessionOn(pubAux.exec);

  doPublishNamespace(firstPublisher, kTestNamespace);

  auto claimingConsumer = createMockConsumer();
  auto parkedConsumer = createMockConsumer();
  auto parkedSubgroup = createMockSubgroupConsumer();
  std::atomic<bool> parkedGotData{false};
  EXPECT_CALL(*parkedConsumer, beginSubgroup(0, 0, _, _))
      .WillOnce([&](uint64_t, uint64_t, uint8_t, moxygen::BeginSubgroupOptions) {
        parkedGotData.store(true);
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(
            parkedSubgroup
        );
      });

  // Park relayExec so the first subscribe holds its claim open for the whole race.
  auto relayGate = parkRelayExec();

  auto claiming =
      launchSubscribeOn(pubEvb, claimingSub, makeSubscribeRequest(RequestID(0)), claimingConsumer);
  auto parked =
      launchSubscribeOn(pubEvb, parkedSub, makeSubscribeRequest(RequestID(1)), parkedConsumer);
  ASSERT_FALSE(parked.ready()) << "the second subscribe should be parked on the claim";

  auto issuePublish = [&](const std::shared_ptr<MoQSession>& session) {
    std::shared_ptr<TrackConsumer> consumer;
    withSessionContext(session, [&]() {
      PublishRequest pub;
      pub.fullTrackName = kTestTrackName;
      auto res = subscriberInterface()->publish(std::move(pub), createMockSubscriptionHandle());
      EXPECT_TRUE(res.hasValue());
      if (res.hasValue()) {
        consumer = res->consumer;
        co_withExecutor(folly::getKeepAliveToken(pubEvb), std::move(res->reply)).start();
      }
    });
    return consumer;
  };

  // One turn, so the waiter released by the first PUBLISH cannot resume before the second.
  std::shared_ptr<TrackConsumer> firstPubConsumer;
  std::shared_ptr<TrackConsumer> secondPubConsumer;
  pubEvb->runInEventBaseThreadAndWait([&]() {
    firstPubConsumer = issuePublish(firstPublisher);
    secondPubConsumer = issuePublish(secondPublisher);
    EXPECT_FALSE(parked.ready()) << "the waiter must not resume inside the turn that wakes it";
  });
  ASSERT_NE(firstPubConsumer, nullptr);
  ASSERT_NE(secondPubConsumer, nullptr);

  for (int i = 0; i < 8 && !parked.ready(); ++i) {
    pubEvb->runInEventBaseThreadAndWait([]() {});
  }
  ASSERT_TRUE(parked.ready()) << "the parked subscribe never resumed";
  auto parkedHandle = parked.handle();
  ASSERT_NE(parkedHandle, nullptr);
  getOrCreateMockState(parkedSub)->subscribeHandles.push_back(parkedHandle);

  // relayExec is still parked, so neither publisher forwarder has drained. Both can still
  // deliver, and only the live one should reach the parked subscriber.
  pubEvb->runInEventBaseThreadAndWait([&]() {
    auto sgRes = secondPubConsumer->beginSubgroup(0, 0, 0);
    ASSERT_TRUE(sgRes.hasValue());
    EXPECT_TRUE(sgRes.value()->endOfSubgroup().hasValue());
  });
  for (int i = 0; i < 4 && !parkedGotData.load(); ++i) {
    pubEvb->runInEventBaseThreadAndWait([]() {});
  }
  EXPECT_TRUE(parkedGotData.load())
      << "the parked subscriber is not attached to the forwarder that holds the track";

  relayGate.release();
  EXPECT_TRUE(driveUntil([&] { return claiming.ready(); }))
      << "the displaced subscribe never unwound";

  pubEvb->runInEventBaseThreadAndWait([&]() {
    removeSession(firstPublisher);
    removeSession(secondPublisher);
    removeSession(claimingSub);
    removeSession(parkedSub);
  });
  drainExecs(4);
}

// The slot can turn over completely while a waiter is waking. Here the PUBLISH that
// releases the waiter publishes done in the same turn, and a SUBSCRIBE queued ahead of the
// waiter claims the empty slot. Finding the entry pending again is another setup to wait
// out, not a failure.
TEST_P(MoQRelayTest, ParkedSubscribeWaitsOutASlotReclaimedWhileItWasWaking) {
  if (relayMode() != RelayMode::LocalForwarderMT) {
    GTEST_SKIP() << "only LF has a per-thread registry slot to contend for";
  }

  auto& pubAux = makeAuxExec("pub-iothread");
  auto* pubEvb = pubAux.evb;
  auto publisherSession = createMockSessionOn(pubAux.exec);
  // Holds the claim the waiter parks on; its own setup is stranded behind the relay gate.
  auto claimingSub = createMockSessionOn(pubAux.exec);
  auto parkedSub = createMockSessionOn(pubAux.exec);
  auto reclaimingSub = createMockSessionOn(pubAux.exec);

  doPublishNamespace(publisherSession, kTestNamespace);

  SubscribeOk upstreamOk;
  upstreamOk.requestID = RequestID(1);
  upstreamOk.trackAlias = TrackAlias(1);
  upstreamOk.expires = std::chrono::milliseconds(0);
  upstreamOk.groupOrder = GroupOrder::OldestFirst;
  std::shared_ptr<TrackConsumer> upstreamConsumer;
  EXPECT_CALL(*publisherSession, subscribe(_, _))
      .WillOnce(
          [&](const SubscribeRequest&, std::shared_ptr<TrackConsumer> consumer
          ) -> folly::coro::Task<Publisher::SubscribeResult> {
            upstreamConsumer = std::move(consumer);
            auto handle = std::make_shared<NiceMock<MockSubscriptionHandle>>(upstreamOk);
            co_return folly::Expected<std::shared_ptr<SubscriptionHandle>, SubscribeError>(handle);
          }
      );

  auto claimingConsumer = createMockConsumer();
  auto parkedConsumer = createMockConsumer();
  auto reclaimingConsumer = createMockConsumer();
  auto parkedSubgroup = createMockSubgroupConsumer();
  // Shares the forwarder, so it gets the same objects. Only the parked one is asserted on.
  auto reclaimingSubgroup = createMockSubgroupConsumer();
  ON_CALL(*reclaimingConsumer, beginSubgroup(_, _, _, _))
      .WillByDefault(
          [reclaimingSubgroup](uint64_t, uint64_t, uint8_t, moxygen::BeginSubgroupOptions) {
            return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(
                reclaimingSubgroup
            );
          }
      );
  std::atomic<bool> parkedGotData{false};
  EXPECT_CALL(*parkedConsumer, beginSubgroup(0, 0, _, _))
      .WillOnce([&](uint64_t, uint64_t, uint8_t, moxygen::BeginSubgroupOptions) {
        parkedGotData.store(true);
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(
            parkedSubgroup
        );
      });

  auto relayGate = parkRelayExec();

  auto claiming =
      launchSubscribeOn(pubEvb, claimingSub, makeSubscribeRequest(RequestID(0)), claimingConsumer);
  auto parked =
      launchSubscribeOn(pubEvb, parkedSub, makeSubscribeRequest(RequestID(1)), parkedConsumer);
  ASSERT_FALSE(parked.ready()) << "the second subscribe should be parked on the claim";

  PendingSubscribe reclaiming;
  std::shared_ptr<TrackConsumer> pubConsumer;
  pubEvb->runInEventBaseThreadAndWait([&]() {
    // Queued ahead of the wake below, so it takes the vacated slot first. Started here
    // rather than with launchSubscribeOn: the turn that wakes the waiter starts it inline.
    reclaiming = startSubscribe(
        pubEvb,
        reclaimingSub,
        makeSubscribeRequest(RequestID(2)),
        reclaimingConsumer
    );
    withSessionContext(publisherSession, [&]() {
      PublishRequest pub;
      pub.fullTrackName = kTestTrackName;
      auto res = subscriberInterface()->publish(std::move(pub), createMockSubscriptionHandle());
      EXPECT_TRUE(res.hasValue());
      if (res.hasValue()) {
        pubConsumer = res->consumer;
        co_withExecutor(folly::getKeepAliveToken(pubEvb), std::move(res->reply)).start();
      }
    });
    ASSERT_NE(pubConsumer, nullptr);
    pubConsumer->publishDone(
        PublishDone{RequestID(0), PublishDoneStatusCode::SUBSCRIPTION_ENDED, 0, "publisher gone"}
    );
    EXPECT_FALSE(parked.ready()) << "the waiter must not resume inside the turn that wakes it";
  });

  for (int i = 0; i < 8; ++i) {
    pubEvb->runInEventBaseThreadAndWait([]() {});
  }
  EXPECT_FALSE(parked.ready()) << "the waiter resolved against a slot still being set up";

  relayGate.release();
  ASSERT_TRUE(driveUntil([&] { return parked.ready() && reclaiming.ready() && claiming.ready(); }))
      << "parked=" << parked.ready() << " reclaiming=" << reclaiming.ready()
      << " claiming=" << claiming.ready();

  auto parkedHandle = parked.handle();
  ASSERT_NE(parkedHandle, nullptr);
  getOrCreateMockState(parkedSub)->subscribeHandles.push_back(parkedHandle);

  ASSERT_NE(upstreamConsumer, nullptr);
  pubEvb->runInEventBaseThreadAndWait([&]() {
    auto sgRes = upstreamConsumer->beginSubgroup(0, 0, 0);
    ASSERT_TRUE(sgRes.hasValue());
    EXPECT_TRUE(sgRes.value()->endOfSubgroup().hasValue());
  });
  EXPECT_TRUE(driveUntil([&] { return parkedGotData.load(); }))
      << "the parked subscriber is not attached to the forwarder that holds the track";

  pubEvb->runInEventBaseThreadAndWait([&]() {
    removeSession(publisherSession);
    removeSession(claimingSub);
    removeSession(parkedSub);
    removeSession(reclaimingSub);
  });
  drainExecs(4);
}

} // namespace openmoq::moqx::test
