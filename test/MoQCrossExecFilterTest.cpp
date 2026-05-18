/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/MoQCrossExecFilter.h"

#include <folly/executors/ManualExecutor.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <moxygen/test/Mocks.h>

using namespace testing;
using namespace moxygen;
using namespace openmoq::moqx;

namespace {

const TrackAlias kAlias{42};

ObjectHeader makeHeader(uint64_t group = 1, uint64_t subgroup = 0, uint64_t id = 0) {
  return ObjectHeader{group, subgroup, id};
}

class MoQCrossExecFilterTest : public ::testing::Test {
protected:
  void SetUp() override {
    innerTrack_ = std::make_shared<NiceMock<MockTrackConsumer>>();
    innerSubgroup_ = std::make_shared<NiceMock<MockSubgroupConsumer>>();

    ON_CALL(*innerTrack_, setTrackAlias(_))
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
    ON_CALL(*innerTrack_, beginSubgroup(_, _, _, _))
        .WillByDefault(Return(folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(innerSubgroup_)));
    ON_CALL(*innerTrack_, objectStream(_, _, _))
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
    ON_CALL(*innerTrack_, datagram(_, _, _))
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
    ON_CALL(*innerTrack_, publishDone(_))
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));

    ON_CALL(*innerSubgroup_, object(_, _, _, _))
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
    ON_CALL(*innerSubgroup_, beginObject(_, _, _, _))
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
    ON_CALL(*innerSubgroup_, objectPayload(_, _))
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(
            ObjectPublishStatus::IN_PROGRESS)));
    ON_CALL(*innerSubgroup_, endOfGroup(_))
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
    ON_CALL(*innerSubgroup_, endOfTrackAndGroup(_))
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
    ON_CALL(*innerSubgroup_, endOfSubgroup())
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));

    filter_ = std::make_shared<MoQCrossExecFilter>(&exec_, innerTrack_);
  }

  folly::ManualExecutor exec_;
  std::shared_ptr<NiceMock<MockTrackConsumer>> innerTrack_;
  std::shared_ptr<NiceMock<MockSubgroupConsumer>> innerSubgroup_;
  std::shared_ptr<MoQCrossExecFilter> filter_;
};

// ---- setTrackAlias ----

TEST_F(MoQCrossExecFilterTest, SetTrackAliasEnqueued) {
  EXPECT_CALL(*innerTrack_, setTrackAlias(_)).Times(0);
  auto result = filter_->setTrackAlias(kAlias);
  EXPECT_TRUE(result.hasValue());

  EXPECT_CALL(*innerTrack_, setTrackAlias(kAlias)).Times(1);
  exec_.drain();
}

// ---- objectStream ----

TEST_F(MoQCrossExecFilterTest, ObjectStreamEnqueued) {
  auto hdr = makeHeader(2, 0, 5);
  EXPECT_CALL(*innerTrack_, objectStream(_, _, _)).Times(0);
  auto result = filter_->objectStream(hdr, nullptr, false);
  EXPECT_TRUE(result.hasValue());

  EXPECT_CALL(*innerTrack_, objectStream(hdr, _, false)).Times(1);
  exec_.drain();
}

// ---- datagram ----

TEST_F(MoQCrossExecFilterTest, DatagramEnqueued) {
  auto hdr = makeHeader(3, 0, 1);
  EXPECT_CALL(*innerTrack_, datagram(_, _, _)).Times(0);
  auto result = filter_->datagram(hdr, nullptr, true);
  EXPECT_TRUE(result.hasValue());

  EXPECT_CALL(*innerTrack_, datagram(hdr, _, true)).Times(1);
  exec_.drain();
}

// ---- publishDone ----

TEST_F(MoQCrossExecFilterTest, PublishDoneEnqueued) {
  PublishDone done;
  done.statusCode = PublishDoneStatusCode::SUBSCRIPTION_ENDED;

  EXPECT_CALL(*innerTrack_, publishDone(_)).Times(0);
  auto result = filter_->publishDone(std::move(done));
  EXPECT_TRUE(result.hasValue());

  EXPECT_CALL(*innerTrack_, publishDone(_)).Times(1);
  exec_.drain();
}

// ---- awaitStreamCredit ----

TEST_F(MoQCrossExecFilterTest, AwaitStreamCreditReturnsReady) {
  auto result = filter_->awaitStreamCredit();
  EXPECT_TRUE(result.hasValue());
  EXPECT_TRUE(result.value().isReady());
}

// ---- beginSubgroup + subgroup methods ----

TEST_F(MoQCrossExecFilterTest, BeginSubgroupReturnsSubgroupImmediately) {
  // beginSubgroup returns a cross-exec wrapper immediately; the inner's
  // beginSubgroup runs only when the target executor is drained.
  auto result = filter_->beginSubgroup(1, 0, 128, false);
  EXPECT_TRUE(result.hasValue());
  ASSERT_NE(result.value(), nullptr);
  // The returned consumer is the cross-exec wrapper, not the inner subgroup
  EXPECT_NE(
      result.value().get(),
      static_cast<moxygen::SubgroupConsumer*>(innerSubgroup_.get()));
  exec_.drain(); // drive the pending beginSubgroup on inner
}

TEST_F(MoQCrossExecFilterTest, BeginSubgroupRunsOnTargetExecutor) {
  EXPECT_CALL(*innerTrack_, beginSubgroup(1, 0, 128, false)).Times(1);
  auto result = filter_->beginSubgroup(1, 0, 128, false);
  EXPECT_TRUE(result.hasValue());
  exec_.drain();
}

TEST_F(MoQCrossExecFilterTest, SubgroupObjectEnqueuedAfterBeginSubgroup) {
  auto subResult = filter_->beginSubgroup(1, 0, 128, false);
  ASSERT_TRUE(subResult.hasValue());
  auto subFilter = subResult.value();

  auto objResult = subFilter->object(0, nullptr, noExtensions(), false);
  EXPECT_TRUE(objResult.hasValue());

  // Before drain: inner not called
  EXPECT_CALL(*innerSubgroup_, object(_, _, _, _)).Times(0);
  EXPECT_CALL(*innerTrack_, beginSubgroup(_, _, _, _)).Times(0);

  // After drain: beginSubgroup runs first, then object
  {
    InSequence seq;
    EXPECT_CALL(*innerTrack_, beginSubgroup(1, 0, 128, false)).Times(1);
    EXPECT_CALL(*innerSubgroup_, object(0, _, _, false)).Times(1);
  }
  exec_.drain();
}

TEST_F(MoQCrossExecFilterTest, SubgroupEndOfSubgroupEnqueued) {
  auto subResult = filter_->beginSubgroup(2, 1, 64, true);
  ASSERT_TRUE(subResult.hasValue());
  auto subFilter = subResult.value();

  subFilter->endOfSubgroup();

  InSequence seq;
  EXPECT_CALL(*innerTrack_, beginSubgroup(2, 1, 64, true)).Times(1);
  EXPECT_CALL(*innerSubgroup_, endOfSubgroup()).Times(1);
  exec_.drain();
}

TEST_F(MoQCrossExecFilterTest, SubgroupEndOfGroupEnqueued) {
  auto subResult = filter_->beginSubgroup(1, 0, 128, false);
  ASSERT_TRUE(subResult.hasValue());
  auto subFilter = subResult.value();

  subFilter->endOfGroup(5);

  InSequence seq;
  EXPECT_CALL(*innerTrack_, beginSubgroup(_, _, _, _)).Times(1);
  EXPECT_CALL(*innerSubgroup_, endOfGroup(5)).Times(1);
  exec_.drain();
}

TEST_F(MoQCrossExecFilterTest, SubgroupEndOfTrackAndGroupEnqueued) {
  auto subResult = filter_->beginSubgroup(1, 0, 128, false);
  ASSERT_TRUE(subResult.hasValue());
  auto subFilter = subResult.value();

  subFilter->endOfTrackAndGroup(7);

  InSequence seq;
  EXPECT_CALL(*innerTrack_, beginSubgroup(_, _, _, _)).Times(1);
  EXPECT_CALL(*innerSubgroup_, endOfTrackAndGroup(7)).Times(1);
  exec_.drain();
}

TEST_F(MoQCrossExecFilterTest, SubgroupResetEnqueued) {
  auto subResult = filter_->beginSubgroup(1, 0, 128, false);
  ASSERT_TRUE(subResult.hasValue());
  auto subFilter = subResult.value();

  subFilter->reset(ResetStreamErrorCode::CANCELLED);

  InSequence seq;
  EXPECT_CALL(*innerTrack_, beginSubgroup(_, _, _, _)).Times(1);
  EXPECT_CALL(*innerSubgroup_, reset(ResetStreamErrorCode::CANCELLED)).Times(1);
  exec_.drain();
}

TEST_F(MoQCrossExecFilterTest, SubgroupBeginObjectEnqueued) {
  auto subResult = filter_->beginSubgroup(1, 0, 128, false);
  ASSERT_TRUE(subResult.hasValue());
  auto subFilter = subResult.value();

  subFilter->beginObject(3, 100, nullptr, noExtensions());

  InSequence seq;
  EXPECT_CALL(*innerTrack_, beginSubgroup(_, _, _, _)).Times(1);
  EXPECT_CALL(*innerSubgroup_, beginObject(3, 100, _, _)).Times(1);
  exec_.drain();
}

TEST_F(MoQCrossExecFilterTest, SubgroupObjectPayloadEnqueued) {
  auto subResult = filter_->beginSubgroup(1, 0, 128, false);
  ASSERT_TRUE(subResult.hasValue());
  auto subFilter = subResult.value();

  // beginObject must precede objectPayload so the byte tracker knows the length.
  subFilter->beginObject(0, 10, nullptr);

  auto payloadResult = subFilter->objectPayload(nullptr, true);
  EXPECT_TRUE(payloadResult.hasValue());
  EXPECT_EQ(payloadResult.value(), ObjectPublishStatus::IN_PROGRESS);

  InSequence seq;
  EXPECT_CALL(*innerTrack_, beginSubgroup(_, _, _, _)).Times(1);
  EXPECT_CALL(*innerSubgroup_, beginObject(0, 10, _, _)).Times(1);
  EXPECT_CALL(*innerSubgroup_, objectPayload(_, true)).Times(1);
  exec_.drain();
}

// Verify FIFO ordering: multiple track-level calls arrive in order
TEST_F(MoQCrossExecFilterTest, MultipleCallsDeliveredInOrder) {
  auto hdr1 = makeHeader(1, 0, 0);
  auto hdr2 = makeHeader(1, 0, 1);

  filter_->objectStream(hdr1, nullptr, false);
  filter_->objectStream(hdr2, nullptr, false);
  filter_->publishDone(PublishDone{});

  InSequence seq;
  EXPECT_CALL(*innerTrack_, objectStream(hdr1, _, false)).Times(1);
  EXPECT_CALL(*innerTrack_, objectStream(hdr2, _, false)).Times(1);
  EXPECT_CALL(*innerTrack_, publishDone(_)).Times(1);
  exec_.drain();
}

} // namespace
