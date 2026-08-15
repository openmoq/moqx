/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Originally from github.com/facebookexperimental/moxygen.
 * See the moxygen LICENSE for the original license terms:
 * https://github.com/openmoq/moxygen/blob/main/LICENSE
 *
 * Copyright (c) OpenMOQ contributors.
 */

#include "MoqxRelayTestFixture.h"

namespace openmoq::moqx::test {

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

  folly::ScopedEventBaseThread pubThread("pub-iothread");
  auto* pubEvb = pubThread.getEventBase();
  auto pubExec = std::make_shared<MoQFollyExecutorImpl>(pubEvb);

  auto makeSessionOnPubThread = [&] {
    auto session = std::make_shared<NiceMock<MockMoQSession>>(pubExec);
    ON_CALL(*session, getNegotiatedVersion())
        .WillByDefault(Return(std::optional<uint64_t>(kVersionDraftCurrent)));
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
  folly::ScopedEventBaseThread subThread("sub2-thread");
  auto subExec = std::make_shared<MoQFollyExecutorImpl>(subThread.getEventBase());
  auto subSession2 = std::make_shared<NiceMock<MockMoQSession>>(subExec);
  ON_CALL(*subSession2, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraftCurrent)));
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
      subThread.getEventBase()->runInEventBaseThreadAndWait([] {});
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
  subThread.getEventBase()->runInEventBaseThreadAndWait([] {});
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
  folly::ScopedEventBaseThread pubThread("pub-iothread");
  auto* pubEvb = pubThread.getEventBase();
  auto pubExec = std::make_shared<MoQFollyExecutorImpl>(pubEvb);
  auto publisherSession = std::make_shared<NiceMock<MockMoQSession>>(pubExec);
  ON_CALL(*publisherSession, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraftCurrent)));
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
} // namespace openmoq::moqx::test
