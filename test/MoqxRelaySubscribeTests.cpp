/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Originally from github.com/facebookexperimental/moxygen.
 * See deps/moxygen/LICENSE for the original license terms.
 *
 * Copyright (c) OpenMOQ contributors.
 */

#include "MoqxRelayTestFixture.h"

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

} // namespace moxygen::test
