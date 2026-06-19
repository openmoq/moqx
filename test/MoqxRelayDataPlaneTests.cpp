/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Originally from github.com/facebookexperimental/moxygen.
 * See deps/moxygen/LICENSE for the original license terms.
 *
 * Copyright (c) OpenMOQ contributors.
 */

#include "MoqxRelayTestFixture.h"

namespace moxygen::test {

// Test: Duplicate beginSubgroup with active consumers resets them and creates
// new ones.
// Sequence: publish, 2 subscribers, beginSubgroup, beginSubgroup again ->
// first consumers get reset, both subscribers get new consumers.
TEST_P(MoQRelayTest, DuplicateSubgroupReplacesActiveConsumers) {
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
      .WillOnce([&](uint64_t, uint64_t, uint8_t, moxygen::BeginSubgroupOptions) {
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(sg1v1);
      })
      .WillOnce([&](uint64_t, uint64_t, uint8_t, moxygen::BeginSubgroupOptions) {
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(sg1v2);
      });
  EXPECT_CALL(*mockConsumer2, beginSubgroup(0, 0, _, _))
      .WillOnce([&](uint64_t, uint64_t, uint8_t, moxygen::BeginSubgroupOptions) {
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(sg2v1);
      })
      .WillOnce([&](uint64_t, uint64_t, uint8_t, moxygen::BeginSubgroupOptions) {
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
  driveIfMultiThread(
  ); // flush so beginSubgroup wires downstream_ and calls mockConsumer beginSubgroup

  // Duplicate beginSubgroup - should reset v1 consumers and return new
  // forwarder. Simulate publisher resetting the old stream.
  auto sgForwarder2 = publishConsumer->beginSubgroup(0, 0, 0);
  driveIfMultiThread(); // flush duplicate so v1 consumers are reset and v2 consumers are created
  (*sgForwarder1)->reset(moxygen::ResetStreamErrorCode::CANCELLED);
  driveIfMultiThread(); // flush publisher reset of old stream
  EXPECT_TRUE(sgForwarder2.hasValue());
  EXPECT_NE(sgForwarder1.value(), sgForwarder2.value());

  // Close the new subgroup cleanly before teardown to avoid reset during
  // cleanup
  EXPECT_TRUE(sgForwarder2.value()->endOfSubgroup().hasValue());
  driveIfMultiThread(); // flush endOfSubgroup

  removeSession(publisherSession);
  removeSession(sub1);
  removeSession(sub2);
  driveIfMultiThread(); // flush relay cleanup so it drops session refs before mocks are destroyed
}

// Test: Duplicate beginSubgroup after all subscribers have stop_sending'd
// returns CANCELLED to propagate the signal back to the publisher.
TEST_P(MoQRelayTest, DuplicateSubgroupCancelledWhenNoActiveConsumers) {
  auto publisherSession = createMockSession();
  auto subscriber = createMockSession();

  auto mockConsumer = createMockConsumer();
  auto mockSg = std::make_shared<NiceMock<MockSubgroupConsumer>>();

  EXPECT_CALL(*mockConsumer, beginSubgroup(0, 0, _, _))
      .WillOnce([&](uint64_t, uint64_t, uint8_t, moxygen::BeginSubgroupOptions) {
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
  driveIfMultiThread(); // flush so beginSubgroup wires sg.downstream_ before object() enqueues

  // Trigger stop_sending tombstone via CANCELLED error from object()
  if (relayEvb_) {
    // MT: enqueue an extra object() after the beginSubgroup lambda
    sg->object(0, nullptr, {}, false);
    driveIfMultiThread(); // flush so object() runs and tombstones the subscriber
  }
  sg->object(0, nullptr, {}, false);
  driveIfMultiThread(); // flush so object() runs and tombstones the subscriber

  // Duplicate beginSubgroup - all consumers tombstoned, should return CANCELLED.
  auto dupRes = publishConsumer->beginSubgroup(0, 0, 0);
  if (relayMode() == RelayMode::MultiThread) {
    // MT mode: a CrossExecFilter sits between publisher and relay, so it always
    // returns a subFilter and the error is deferred until the next operation.
    ASSERT_TRUE(dupRes.hasValue());
    driveIfMultiThread(); // flush so object() runs and tombstones the subscriber
    auto probeRes = dupRes.value()->endOfSubgroup();
    EXPECT_TRUE(probeRes.hasError());
    if (probeRes.hasError()) {
      EXPECT_EQ(probeRes.error().code, MoQPublishError::CANCELLED);
    }
  } else {
    // ST and LocalForwarderMT: the publisher writes directly to the (local)
    // forwarder with no cross-exec hop, so CANCELLED is returned synchronously.
    EXPECT_TRUE(dupRes.hasError());
    EXPECT_EQ(dupRes.error().code, MoQPublishError::CANCELLED);
  }

  removeSession(publisherSession);
  removeSession(subscriber);
  sg->reset(ResetStreamErrorCode::CANCELLED);
  driveIfMultiThread(); // flush relay cleanup so it drops session refs before mocks are destroyed
}

// Test: Duplicate beginSubgroup with partial stop_sending - active subscriber
// gets reset and new consumer; tombstoned subscriber is skipped.
TEST_P(MoQRelayTest, DuplicateSubgroupSkipsTombstonedSubscriber) {
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
      .WillOnce([&](uint64_t, uint64_t, uint8_t, moxygen::BeginSubgroupOptions) {
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(sgAv1);
      })
      .WillOnce([&](uint64_t, uint64_t, uint8_t, moxygen::BeginSubgroupOptions) {
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(sgAv2);
      });
  EXPECT_CALL(*consumerB, beginSubgroup(0, 0, _, _))
      .WillOnce([&](uint64_t, uint64_t, uint8_t, moxygen::BeginSubgroupOptions) {
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(sgBv1);
      });

  // object() is forwarded to both A and B; sub A succeeds, sub B returns
  // CANCELLED to simulate stop_sending
  EXPECT_CALL(*sgAv1, object(_, _, _, _))
      .WillOnce(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
  if (relayEvb_) {
    EXPECT_CALL(*sgAv1, object(_, _, _, _))
        .WillOnce(Return(folly::makeExpected<MoQPublishError>(folly::unit)))
        .RetiresOnSaturation();
  }
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
  driveIfMultiThread(); // flush so beginSubgroup wires downstream_ before object() enqueues

  if (relayEvb_) {
    // MT: enqueue an extra object() after the beginSubgroup lambda
    sgForwarder1.value()->object(0, nullptr, {}, false);
    driveIfMultiThread(); // flush so object() runs and tombstones the subscriber
  }
  // Trigger tombstone for sub B via CANCELLED from object()
  sgForwarder1.value()->object(0, nullptr, {}, false);
  driveIfMultiThread(); // flush so object() runs and tombstones sub B

  // Duplicate beginSubgroup: sub A gets reset+new, sub B is skipped
  // (tombstoned). Simulate publisher resetting the old stream.
  auto sgForwarder2 = publishConsumer->beginSubgroup(0, 0, 0);
  driveIfMultiThread(); // flush duplicate so sgAv1 is reset and sgAv2 is created
  (*sgForwarder1)->reset(moxygen::ResetStreamErrorCode::CANCELLED);
  driveIfMultiThread(); // flush publisher reset of old stream
  EXPECT_TRUE(sgForwarder2.hasValue());
  EXPECT_NE(sgForwarder1.value(), sgForwarder2.value());

  // Close the new subgroup cleanly before teardown
  EXPECT_TRUE(sgForwarder2.value()->endOfSubgroup().hasValue());
  driveIfMultiThread(); // flush endOfSubgroup

  removeSession(publisherSession);
  removeSession(subA);
  removeSession(subB);
  driveIfMultiThread(); // flush relay cleanup so it drops session refs before mocks are destroyed
}

} // namespace moxygen::test
