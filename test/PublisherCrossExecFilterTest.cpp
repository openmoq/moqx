/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/PublisherCrossExecFilter.h"

#include <folly/coro/BlockingWait.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/ManualExecutor.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <moxygen/test/Mocks.h>

using namespace testing;
using namespace moxygen;
using namespace openmoq::moqx;

namespace {

class PublisherCrossExecFilterTest : public ::testing::Test {
protected:
  void SetUp() override {
    targetExec_ = std::make_shared<folly::CPUThreadPoolExecutor>(1);
    inner_ = std::make_shared<NiceMock<MockPublisher>>();
    filter_ = std::make_shared<PublisherCrossExecFilter>(targetExec_.get(), inner_);
  }

  std::shared_ptr<folly::CPUThreadPoolExecutor> targetExec_;
  std::shared_ptr<NiceMock<MockPublisher>> inner_;
  std::shared_ptr<PublisherCrossExecFilter> filter_;
};

// ---- subscribe ----

TEST_F(PublisherCrossExecFilterTest, SubscribeForwardsToInner) {
  SubscribeOk ok;
  ok.requestID = RequestID(1);
  auto handle = std::make_shared<NiceMock<MockSubscriptionHandle>>(ok);
  EXPECT_CALL(*inner_, subscribe(_, _))
      .WillOnce(
          [handle](SubscribeRequest, std::shared_ptr<TrackConsumer>)
              -> folly::coro::Task<Publisher::SubscribeResult> {
            co_return folly::makeExpected<SubscribeError>(std::shared_ptr<SubscriptionHandle>(handle
            ));
          }
      );

  SubscribeRequest sub;
  sub.requestID = RequestID(1);
  auto result = folly::coro::blockingWait(filter_->subscribe(std::move(sub), nullptr));
  EXPECT_TRUE(result.hasValue());
}

TEST_F(PublisherCrossExecFilterTest, SubscribeReturnsError) {
  EXPECT_CALL(*inner_, subscribe(_, _))
      .WillOnce(
          [](SubscribeRequest sub,
             std::shared_ptr<TrackConsumer>) -> folly::coro::Task<Publisher::SubscribeResult> {
            co_return folly::makeUnexpected(
                SubscribeError{sub.requestID, SubscribeErrorCode::NOT_SUPPORTED, "nope"}
            );
          }
      );

  SubscribeRequest sub;
  sub.requestID = RequestID(2);
  auto result = folly::coro::blockingWait(filter_->subscribe(std::move(sub), nullptr));
  EXPECT_FALSE(result.hasValue());
  EXPECT_EQ(result.error().errorCode, SubscribeErrorCode::NOT_SUPPORTED);
}

// ---- trackStatus ----

TEST_F(PublisherCrossExecFilterTest, TrackStatusForwardsToInner) {
  EXPECT_CALL(*inner_, trackStatus(_))
      .WillOnce([](TrackStatus ts) -> folly::coro::Task<Publisher::TrackStatusResult> {
        TrackStatusOk ok;
        ok.requestID = ts.requestID;
        co_return folly::makeExpected<TrackStatusError>(std::move(ok));
      });

  TrackStatus ts;
  ts.requestID = RequestID(3);
  auto result = folly::coro::blockingWait(filter_->trackStatus(std::move(ts)));
  EXPECT_TRUE(result.hasValue());
}

TEST_F(PublisherCrossExecFilterTest, TrackStatusReturnsError) {
  EXPECT_CALL(*inner_, trackStatus(_))
      .WillOnce([](TrackStatus ts) -> folly::coro::Task<Publisher::TrackStatusResult> {
        co_return folly::makeUnexpected(
            TrackStatusError{ts.requestID, TrackStatusErrorCode::NOT_SUPPORTED, "nope"}
        );
      });

  TrackStatus ts;
  ts.requestID = RequestID(3);
  auto result = folly::coro::blockingWait(filter_->trackStatus(std::move(ts)));
  EXPECT_FALSE(result.hasValue());
  EXPECT_EQ(result.error().requestID, RequestID(3));
}

// ---- fetch ----

TEST_F(PublisherCrossExecFilterTest, FetchForwardsToInner) {
  EXPECT_CALL(*inner_, fetch(_, _))
      .WillOnce(
          [](Fetch, std::shared_ptr<FetchConsumer>) -> folly::coro::Task<Publisher::FetchResult> {
            co_return folly::makeUnexpected(
                FetchError{RequestID(4), FetchErrorCode::NOT_SUPPORTED, "nope"}
            );
          }
      );

  Fetch fetchReq;
  fetchReq.requestID = RequestID(4);
  auto result = folly::coro::blockingWait(filter_->fetch(std::move(fetchReq), nullptr));
  EXPECT_FALSE(result.hasValue());
  EXPECT_EQ(result.error().errorCode, FetchErrorCode::NOT_SUPPORTED);
}

TEST_F(PublisherCrossExecFilterTest, FetchReturnsSuccess) {
  FetchOk ok;
  ok.requestID = RequestID(4);
  auto handle = std::make_shared<NiceMock<MockFetchHandle>>(ok);
  EXPECT_CALL(*inner_, fetch(_, _))
      .WillOnce(
          [handle](Fetch, std::shared_ptr<FetchConsumer>)
              -> folly::coro::Task<Publisher::FetchResult> {
            co_return folly::makeExpected<FetchError>(std::shared_ptr<Publisher::FetchHandle>(handle
            ));
          }
      );

  Fetch fetchReq;
  fetchReq.requestID = RequestID(4);
  auto result = folly::coro::blockingWait(filter_->fetch(std::move(fetchReq), nullptr));
  ASSERT_TRUE(result.hasValue());
  ASSERT_NE(result.value(), nullptr);
  EXPECT_EQ(result.value()->fetchOk().requestID, RequestID(4));
}

// ---- subscribeNamespace ----

TEST_F(PublisherCrossExecFilterTest, SubscribeNamespaceForwardsToInner) {
  EXPECT_CALL(*inner_, subscribeNamespace(_, _))
      .WillOnce(
          [](SubscribeNamespace subAnn, std::shared_ptr<Publisher::NamespacePublishHandle>)
              -> folly::coro::Task<Publisher::SubscribeNamespaceResult> {
            co_return folly::makeUnexpected(SubscribeNamespaceError{
                subAnn.requestID,
                SubscribeNamespaceErrorCode::NOT_SUPPORTED,
                "nope"
            });
          }
      );

  SubscribeNamespace subAnn;
  subAnn.requestID = RequestID(5);
  auto result = folly::coro::blockingWait(filter_->subscribeNamespace(std::move(subAnn), nullptr));
  EXPECT_FALSE(result.hasValue());
}

TEST_F(PublisherCrossExecFilterTest, SubscribeNamespaceReturnsSuccess) {
  auto handle = std::make_shared<NiceMock<MockSubscribeNamespaceHandle>>();
  EXPECT_CALL(*inner_, subscribeNamespace(_, _))
      .WillOnce(
          [handle](SubscribeNamespace, std::shared_ptr<Publisher::NamespacePublishHandle>)
              -> folly::coro::Task<Publisher::SubscribeNamespaceResult> {
            co_return folly::makeExpected<SubscribeNamespaceError>(
                std::shared_ptr<Publisher::SubscribeNamespaceHandle>(handle)
            );
          }
      );

  SubscribeNamespace subAnn;
  subAnn.requestID = RequestID(5);
  auto result = folly::coro::blockingWait(filter_->subscribeNamespace(std::move(subAnn), nullptr));
  ASSERT_TRUE(result.hasValue());
  EXPECT_NE(result.value(), nullptr);
}

// ---- goaway ----

TEST_F(PublisherCrossExecFilterTest, GoawayEnqueued) {
  folly::ManualExecutor manualExec;
  auto filter = std::make_shared<PublisherCrossExecFilter>(&manualExec, inner_);

  EXPECT_CALL(*inner_, goaway(_)).Times(0);
  filter->goaway(Goaway{});

  EXPECT_CALL(*inner_, goaway(_)).Times(1);
  manualExec.drain();
}

} // namespace
