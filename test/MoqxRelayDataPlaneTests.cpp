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

} // namespace moxygen::test
