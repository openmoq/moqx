/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/CrossExecFilter.h"

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

class CrossExecFilterTest : public ::testing::Test {
protected:
  void SetUp() override {
    innerTrack_ = std::make_shared<NiceMock<MockTrackConsumer>>();
    innerSubgroup_ = std::make_shared<NiceMock<MockSubgroupConsumer>>();

    ON_CALL(*innerTrack_, setTrackAlias(_))
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
    ON_CALL(*innerTrack_, beginSubgroup(_, _, _, _))
        .WillByDefault(Return(
            folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(innerSubgroup_)
        ));
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
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(ObjectPublishStatus::IN_PROGRESS)
        ));
    ON_CALL(*innerSubgroup_, endOfGroup(_))
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
    ON_CALL(*innerSubgroup_, endOfTrackAndGroup(_))
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
    ON_CALL(*innerSubgroup_, endOfSubgroup())
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));

    filter_ = std::make_shared<CrossExecFilter>(&exec_, innerTrack_);
  }

  folly::ManualExecutor exec_;
  std::shared_ptr<NiceMock<MockTrackConsumer>> innerTrack_;
  std::shared_ptr<NiceMock<MockSubgroupConsumer>> innerSubgroup_;
  std::shared_ptr<CrossExecFilter> filter_;
};

// ---- setTrackAlias ----

TEST_F(CrossExecFilterTest, SetTrackAliasEnqueued) {
  EXPECT_CALL(*innerTrack_, setTrackAlias(_)).Times(0);
  auto result = filter_->setTrackAlias(kAlias);
  EXPECT_TRUE(result.hasValue());

  EXPECT_CALL(*innerTrack_, setTrackAlias(kAlias)).Times(1);
  exec_.drain();
}

// ---- objectStream ----

TEST_F(CrossExecFilterTest, ObjectStreamEnqueued) {
  auto hdr = makeHeader(2, 0, 5);
  EXPECT_CALL(*innerTrack_, objectStream(_, _, _)).Times(0);
  auto result = filter_->objectStream(hdr, nullptr, false);
  EXPECT_TRUE(result.hasValue());

  EXPECT_CALL(*innerTrack_, objectStream(hdr, _, false)).Times(1);
  exec_.drain();
}

// ---- datagram ----

TEST_F(CrossExecFilterTest, DatagramEnqueued) {
  auto hdr = makeHeader(3, 0, 1);
  EXPECT_CALL(*innerTrack_, datagram(_, _, _)).Times(0);
  auto result = filter_->datagram(hdr, nullptr, true);
  EXPECT_TRUE(result.hasValue());

  EXPECT_CALL(*innerTrack_, datagram(hdr, _, true)).Times(1);
  exec_.drain();
}

// ---- publishDone ----

TEST_F(CrossExecFilterTest, PublishDoneEnqueued) {
  PublishDone done;
  done.statusCode = PublishDoneStatusCode::SUBSCRIPTION_ENDED;

  EXPECT_CALL(*innerTrack_, publishDone(_)).Times(0);
  auto result = filter_->publishDone(std::move(done));
  EXPECT_TRUE(result.hasValue());

  EXPECT_CALL(*innerTrack_, publishDone(_)).Times(1);
  exec_.drain();
}

// ---- awaitStreamCredit ----

TEST_F(CrossExecFilterTest, AwaitStreamCreditReturnsReady) {
  auto result = filter_->awaitStreamCredit();
  EXPECT_TRUE(result.hasValue());
  EXPECT_TRUE(result.value().isReady());
}

// ---- beginSubgroup + subgroup methods ----

TEST_F(CrossExecFilterTest, BeginSubgroupReturnsSubgroupImmediately) {
  // beginSubgroup returns a cross-exec wrapper immediately; the inner's
  // beginSubgroup runs only when the target executor is drained.
  auto result = filter_->beginSubgroup(1, 0, 128, false);
  EXPECT_TRUE(result.hasValue());
  ASSERT_NE(result.value(), nullptr);
  // The returned consumer is the cross-exec wrapper, not the inner subgroup
  EXPECT_NE(result.value().get(), static_cast<moxygen::SubgroupConsumer*>(innerSubgroup_.get()));
  exec_.drain(); // drive the pending beginSubgroup on inner
}

TEST_F(CrossExecFilterTest, BeginSubgroupRunsOnTargetExecutor) {
  EXPECT_CALL(*innerTrack_, beginSubgroup(1, 0, 128, false)).Times(1);
  auto result = filter_->beginSubgroup(1, 0, 128, false);
  EXPECT_TRUE(result.hasValue());
  exec_.drain();
}

TEST_F(CrossExecFilterTest, SubgroupObjectEnqueuedAfterBeginSubgroup) {
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

TEST_F(CrossExecFilterTest, SubgroupEndOfSubgroupEnqueued) {
  auto subResult = filter_->beginSubgroup(2, 1, 64, true);
  ASSERT_TRUE(subResult.hasValue());
  auto subFilter = subResult.value();

  subFilter->endOfSubgroup();

  InSequence seq;
  EXPECT_CALL(*innerTrack_, beginSubgroup(2, 1, 64, true)).Times(1);
  EXPECT_CALL(*innerSubgroup_, endOfSubgroup()).Times(1);
  exec_.drain();
}

TEST_F(CrossExecFilterTest, SubgroupEndOfGroupEnqueued) {
  auto subResult = filter_->beginSubgroup(1, 0, 128, false);
  ASSERT_TRUE(subResult.hasValue());
  auto subFilter = subResult.value();

  subFilter->endOfGroup(5);

  InSequence seq;
  EXPECT_CALL(*innerTrack_, beginSubgroup(_, _, _, _)).Times(1);
  EXPECT_CALL(*innerSubgroup_, endOfGroup(5)).Times(1);
  exec_.drain();
}

TEST_F(CrossExecFilterTest, SubgroupEndOfTrackAndGroupEnqueued) {
  auto subResult = filter_->beginSubgroup(1, 0, 128, false);
  ASSERT_TRUE(subResult.hasValue());
  auto subFilter = subResult.value();

  subFilter->endOfTrackAndGroup(7);

  InSequence seq;
  EXPECT_CALL(*innerTrack_, beginSubgroup(_, _, _, _)).Times(1);
  EXPECT_CALL(*innerSubgroup_, endOfTrackAndGroup(7)).Times(1);
  exec_.drain();
}

TEST_F(CrossExecFilterTest, SubgroupResetEnqueued) {
  auto subResult = filter_->beginSubgroup(1, 0, 128, false);
  ASSERT_TRUE(subResult.hasValue());
  auto subFilter = subResult.value();

  subFilter->reset(ResetStreamErrorCode::CANCELLED);

  InSequence seq;
  EXPECT_CALL(*innerTrack_, beginSubgroup(_, _, _, _)).Times(1);
  EXPECT_CALL(*innerSubgroup_, reset(ResetStreamErrorCode::CANCELLED)).Times(1);
  exec_.drain();
}

TEST_F(CrossExecFilterTest, SubgroupBeginObjectEnqueued) {
  auto subResult = filter_->beginSubgroup(1, 0, 128, false);
  ASSERT_TRUE(subResult.hasValue());
  auto subFilter = subResult.value();

  subFilter->beginObject(3, 100, nullptr, noExtensions());

  InSequence seq;
  EXPECT_CALL(*innerTrack_, beginSubgroup(_, _, _, _)).Times(1);
  EXPECT_CALL(*innerSubgroup_, beginObject(3, 100, _, _)).Times(1);
  exec_.drain();
}

TEST_F(CrossExecFilterTest, SubgroupObjectPayloadEnqueued) {
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
TEST_F(CrossExecFilterTest, MultipleCallsDeliveredInOrder) {
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

// objectStream failure bumps the counter but does NOT gate the track.
TEST_F(CrossExecFilterTest, ObjectStreamErrorBumpsCounterDoesNotGateTrack) {
  EXPECT_CALL(*innerTrack_, objectStream(_, _, _))
      .WillOnce(Return(folly::makeUnexpected(MoQPublishError(MoQPublishError::Code::WRITE_ERROR))));

  filter_->objectStream(makeHeader(), nullptr);
  EXPECT_EQ(filter_->objectStreamErrors(), 0u); // counter updated on target exec, not yet

  exec_.drain(); // lambda runs; inner returns error; counter bumped
  EXPECT_EQ(filter_->objectStreamErrors(), 1u);

  // Track-level gate is NOT set — beginSubgroup still works.
  EXPECT_CALL(*innerTrack_, beginSubgroup(_, _, _, _)).Times(1);
  auto subResult = filter_->beginSubgroup(1, 0, 128, false);
  EXPECT_TRUE(subResult.hasValue());
  exec_.drain();
}

// datagram failure bumps the counter but does NOT gate objectStream or beginSubgroup.
TEST_F(CrossExecFilterTest, DatagramErrorBumpsCounterDoesNotGateTrack) {
  EXPECT_CALL(*innerTrack_, datagram(_, _, _))
      .WillOnce(Return(folly::makeUnexpected(MoQPublishError(MoQPublishError::Code::WRITE_ERROR))));

  filter_->datagram(makeHeader(3, 0, 1), nullptr, false);
  exec_.drain();
  EXPECT_EQ(filter_->datagramErrors(), 1u);
  EXPECT_EQ(filter_->objectStreamErrors(), 0u);

  // objectStream and beginSubgroup are unaffected.
  EXPECT_CALL(*innerTrack_, objectStream(_, _, _)).Times(1);
  EXPECT_TRUE(filter_->objectStream(makeHeader(3, 0, 2), nullptr, false).hasValue());

  EXPECT_CALL(*innerTrack_, beginSubgroup(_, _, _, _)).Times(1);
  EXPECT_TRUE(filter_->beginSubgroup(1, 0, 128, false).hasValue());

  exec_.drain();
}

// beginSubgroup failure stores the error on the subgroup, not the track.
// Subsequent subgroup calls fail immediately; other track calls still work.
TEST_F(CrossExecFilterTest, BeginSubgroupFailureGatesSubgroupNotTrack) {
  EXPECT_CALL(*innerTrack_, beginSubgroup(_, _, _, _))
      .WillOnce(Return(folly::makeUnexpected(MoQPublishError(MoQPublishError::Code::WRITE_ERROR))));

  auto subResult = filter_->beginSubgroup(1, 0, 128, false);
  ASSERT_TRUE(subResult.hasValue());
  auto subFilter = subResult.value();

  exec_.drain(); // inner beginSubgroup fails; error stored on subFilter

  // Subsequent subgroup call fails immediately from the deferred error.
  auto objResult = subFilter->object(0, nullptr, noExtensions(), false);
  EXPECT_TRUE(objResult.hasError());

  // Track-level gate is NOT set — a new beginSubgroup still works.
  EXPECT_CALL(*innerTrack_, beginSubgroup(_, _, _, _)).Times(1);
  auto subResult2 = filter_->beginSubgroup(2, 0, 128, false);
  EXPECT_TRUE(subResult2.hasValue());
  exec_.drain();
}

// ---- FetchCrossExecFilter ----

class FetchCrossExecFilterTest : public ::testing::Test {
protected:
  void SetUp() override {
    inner_ = std::make_shared<NiceMock<MockFetchConsumer>>();
    ON_CALL(*inner_, object(_, _, _, _, _, _, _))
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
    ON_CALL(*inner_, endOfGroup(_, _, _, _))
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
    ON_CALL(*inner_, endOfTrackAndGroup(_, _, _))
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
    ON_CALL(*inner_, endOfFetch())
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
    ON_CALL(*inner_, endOfUnknownRange(_, _, _))
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
    filter_ = std::make_shared<FetchCrossExecFilter>(&exec_, inner_);
  }

  folly::ManualExecutor exec_;
  std::shared_ptr<NiceMock<MockFetchConsumer>> inner_;
  std::shared_ptr<FetchCrossExecFilter> filter_;
};

TEST_F(FetchCrossExecFilterTest, MultipleCallsDeliveredInOrder) {
  filter_->object(1, 0, 0, nullptr, noExtensions(), false, false);
  filter_->checkpoint();
  filter_->object(1, 0, 1, nullptr, noExtensions(), false, false);
  filter_->endOfFetch();

  InSequence seq;
  EXPECT_CALL(*inner_, object(1, 0, 0, _, _, false, false)).Times(1);
  EXPECT_CALL(*inner_, checkpoint()).Times(1);
  EXPECT_CALL(*inner_, object(1, 0, 1, _, _, false, false)).Times(1);
  EXPECT_CALL(*inner_, endOfFetch()).Times(1);
  exec_.drain();
}

TEST_F(FetchCrossExecFilterTest, EndOfUnknownRangeEnqueued) {
  filter_->object(1, 0, 0, nullptr, noExtensions(), false, false);
  filter_->endOfUnknownRange(1, 1, false);
  filter_->endOfFetch();

  InSequence seq;
  EXPECT_CALL(*inner_, object(1, 0, 0, _, _, false, false)).Times(1);
  EXPECT_CALL(*inner_, endOfUnknownRange(1, 1, false)).Times(1);
  EXPECT_CALL(*inner_, endOfFetch()).Times(1);
  exec_.drain();
}

} // namespace
