/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Originally from github.com/facebookexperimental/moxygen.
 * See deps/moxygen/LICENSE for the original license terms.
 *
 * Copyright (c) OpenMOQ contributors.
 */

#include "MoqxRelayTestFixture.h"

#include <folly/ScopeGuard.h>
#include <folly/coro/Baton.h>
#include <folly/coro/Invoke.h>
#include <folly/synchronization/Baton.h>

namespace moxygen::test {

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
// and, without a readiness gate, reads the not-yet-seeded localFwd largest (empty),
// which a client reads as a track restart. ST/MT wait on the registry promise and
// were already correct, so this passes in every mode and regresses only LF.
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
         "pre-seeding value a client reads as a track restart";

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

// A PUBLISH on the subscriber's iothread claims the tl slot with ready=true, satisfying the
// readiness promise an in-flight subscribe still owns. The subscribe must not fulfill it a
// second time and fault a client addSubscriber has already attached.
//
// The publish reply is left un-awaited on purpose: registerPublishOnRelayExec replaces the
// subscribe-path registry entry, failing completeUpstreamSubscription and short-circuiting
// subscribe before the second fulfil.
TEST_P(MoQRelayTest, PublishDuringSubscribeDoesNotDoubleFulfillReadyGate) {
  if (relayMode() != RelayMode::LocalForwarderMT) {
    GTEST_SKIP() << "the readiness gate only exists on the LF subscribe path";
  }
  auto publisherSession = createMockSession();
  auto subSession = createMockSession();

  doPublishNamespace(publisherSession, kTestNamespace);

  SubscribeOk upstreamOk;
  upstreamOk.requestID = RequestID(1);
  upstreamOk.trackAlias = TrackAlias(1);
  upstreamOk.expires = std::chrono::milliseconds(0);
  upstreamOk.groupOrder = GroupOrder::OldestFirst;
  upstreamOk.largest = AbsoluteLocation{3, 0};

  std::shared_ptr<TrackConsumer> publishedConsumer;
  EXPECT_CALL(*publisherSession, subscribe(_, _))
      .WillOnce(
          [&](const SubscribeRequest&,
              std::shared_ptr<TrackConsumer>) -> folly::coro::Task<Publisher::SubscribeResult> {
            // On publisherExec inside the first-subscriber sortie: the tl slot is claimed
            // and the subscriber still owns the gate.
            withSessionContext(publisherSession, [&]() {
              PublishRequest pub;
              pub.fullTrackName = kTestTrackName;
              auto res =
                  subscriberInterface()->publish(std::move(pub), createMockSubscriptionHandle());
              ASSERT_TRUE(res.hasValue()) << "publish in mock unexpectedly failed";
              publishedConsumer = res->consumer;
            });
            auto handle = std::make_shared<NiceMock<MockSubscriptionHandle>>(upstreamOk);
            co_return folly::Expected<std::shared_ptr<SubscriptionHandle>, SubscribeError>(handle);
          }
      );

  std::optional<Publisher::SubscribeResult> result;
  std::string thrown;
  withSessionContext(subSession, [&]() {
    SubscribeRequest sub;
    sub.fullTrackName = kTestTrackName;
    sub.requestID = RequestID(0);
    sub.locType = LocationType::LargestObject;
    auto task = publisherInterface()->subscribe(std::move(sub), createMockConsumer());
    try {
      result = folly::coro::blockingWait(std::move(task), exec_.get());
    } catch (const std::exception& e) {
      thrown = e.what();
    }
  });

  EXPECT_TRUE(thrown.empty()) << "subscribe threw instead of completing: " << thrown;
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result->hasValue()
  ) << "a same-thread publish claiming the tl slot must not fault the subscriber that "
       "owns the gate";
  getOrCreateMockState(subSession)->subscribeHandles.push_back(result->value());

  if (publishedConsumer) {
    publishedConsumer->publishDone(
        {RequestID(0), PublishDoneStatusCode::SESSION_CLOSED, 0, "test cleanup"}
    );
  }
  removeSession(publisherSession);
  removeSession(subSession);
  driveIfMultiThread();
}

// set(ready=false) leaves a fresh tl entry with no gate armed. A subscriber landing on the
// publisher's iothread during the upstream SUBSCRIBE then takes the isNew=false fast path
// and attaches to a publisherFwd whose largest is unseeded — a track restart to the client.
//
// tlForwarders_ is thread-local and the fixture puts every session on exec_, so the
// publisher needs its own iothread; otherwise both registries coincide and the gate covers
// this case.
TEST_P(MoQRelayTest, SubscriberOnPublisherThreadWaitsForUpstreamLargestSeeding) {
  if (relayMode() != RelayMode::LocalForwarderMT) {
    GTEST_SKIP() << "publisher/subscriber iothread split is LF-only";
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

  const AbsoluteLocation kLargest{3, 0};
  SubscribeOk upstreamOk;
  upstreamOk.requestID = RequestID(1);
  upstreamOk.trackAlias = TrackAlias(1);
  upstreamOk.expires = std::chrono::milliseconds(0);
  upstreamOk.groupOrder = GroupOrder::OldestFirst;
  upstreamOk.largest = kLargest;

  // Park the upstream SUBSCRIBE on pubEvb so the second subscriber lands after the tl slot
  // is claimed but before the OK seeds largest.
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
  // Never leave the mock parked on a destroyed Baton if an assertion below returns early.
  auto releaseGate = folly::makeGuard([&]() noexcept { upstreamGate.post(); });

  auto pump = [&](auto pred) {
    for (int i = 0; i < 1000 && !pred(); ++i) {
      exec_->drive();
    }
    return pred();
  };

  auto firstResult = std::make_shared<std::optional<Publisher::SubscribeResult>>();
  withSessionContext(subSession1, [&]() {
    SubscribeRequest sub;
    sub.fullTrackName = kTestTrackName;
    sub.requestID = RequestID(0);
    sub.locType = LocationType::LargestObject;
    auto task = publisherInterface()->subscribe(std::move(sub), createMockConsumer());
    co_withExecutor(
        static_cast<folly::DrivableExecutor*>(exec_.get()),
        folly::coro::co_invoke(
            [t = std::move(task), firstResult]() mutable -> folly::coro::Task<void> {
              *firstResult = co_await std::move(t);
            }
        )
    ).start();
  });
  ASSERT_TRUE(pump([&] { return upstreamSubscribeCalled.load(); }))
      << "relay should issue an upstream subscribe and suspend in it";

  // Build the consumer here: gmock objects must not be created while pubEvb touches mocks.
  auto secondConsumer = createMockConsumer();
  auto secondResult = std::make_shared<std::optional<Publisher::SubscribeResult>>();
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
              [t = std::move(task), secondResult, &secondDone](
              ) mutable -> folly::coro::Task<void> {
                *secondResult = co_await std::move(t);
                secondDone.post();
              }
          )
      ).start();
    });
  });
  // FIFO barrier: the subscribe above has reached its first suspension, so it is past
  // acquireLocalForwarder before the gate opens.
  pubEvb->runInEventBaseThreadAndWait([]() {});

  releaseGate.dismiss();
  upstreamGate.post();
  ASSERT_TRUE(pump([&] { return firstResult->has_value(); }));
  ASSERT_TRUE(secondDone.try_wait_for(std::chrono::seconds(5)))
      << "second subscriber never completed";

  ASSERT_TRUE(firstResult->value().hasValue());
  EXPECT_EQ(firstResult->value().value()->subscribeOk().largest, kLargest);
  ASSERT_TRUE(secondResult->value().hasValue());
  EXPECT_EQ(secondResult->value().value()->subscribeOk().largest, kLargest)
      << "a subscriber landing on the publisher's iothread while the upstream SUBSCRIBE is "
         "in flight must wait for the seeded largest, not read the empty one";

  getOrCreateMockState(subSession1)->subscribeHandles.push_back(firstResult->value().value());
  getOrCreateMockState(subSessionOnPubThread)
      ->subscribeHandles.push_back(secondResult->value().value());

  removeSession(publisherSession);
  removeSession(subSession1);
  removeSession(subSessionOnPubThread);
  // Teardown bounces between relayExec_ and pubEvb; flushing one leaves session refs alive
  // and gmock reports leaked mocks. Also quiesces pubEvb before pubThread joins.
  for (int i = 0; i < 4; ++i) {
    driveIfMultiThread();
    pubEvb->runInEventBaseThreadAndWait([]() {});
  }
}

} // namespace moxygen::test
