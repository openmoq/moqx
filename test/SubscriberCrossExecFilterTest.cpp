/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/SubscriberCrossExecFilter.h"

#include <folly/coro/BlockingWait.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/ManualExecutor.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <future>
#include <moxygen/test/Mocks.h>

using namespace testing;
using namespace moxygen;
using namespace openmoq::moqx;

namespace {

class MockSubscriberWithGoaway : public MockSubscriber {
public:
  MOCK_METHOD(void, goaway, (Goaway), (override));
};

class SubscriberCrossExecFilterTest : public ::testing::Test {
protected:
  void SetUp() override {
    targetExec_ = std::make_shared<folly::CPUThreadPoolExecutor>(1);
    inner_ = std::make_shared<NiceMock<MockSubscriberWithGoaway>>();
    filter_ = std::make_shared<SubscriberCrossExecFilter>(targetExec_.get(), inner_);
  }

  std::shared_ptr<folly::CPUThreadPoolExecutor> targetExec_;
  std::shared_ptr<NiceMock<MockSubscriberWithGoaway>> inner_;
  std::shared_ptr<SubscriberCrossExecFilter> filter_;
};

// ---- publishNamespace ----

TEST_F(SubscriberCrossExecFilterTest, PublishNamespaceForwardsToInner) {
  auto nsHandle = std::make_shared<NiceMock<MockPublishNamespaceHandle>>();
  EXPECT_CALL(*inner_, publishNamespace(_, _))
      .WillOnce(
          [nsHandle](PublishNamespace, std::shared_ptr<Subscriber::PublishNamespaceCallback>)
              -> folly::coro::Task<Subscriber::PublishNamespaceResult> {
            co_return folly::makeExpected<PublishNamespaceError>(
                std::shared_ptr<Subscriber::PublishNamespaceHandle>(nsHandle)
            );
          }
      );

  PublishNamespace ann;
  ann.requestID = RequestID(1);
  auto result = folly::coro::blockingWait(filter_->publishNamespace(std::move(ann), nullptr));
  EXPECT_TRUE(result.hasValue());
}

TEST_F(SubscriberCrossExecFilterTest, PublishNamespaceReturnsError) {
  EXPECT_CALL(*inner_, publishNamespace(_, _))
      .WillOnce(
          [](PublishNamespace ann, std::shared_ptr<Subscriber::PublishNamespaceCallback>)
              -> folly::coro::Task<Subscriber::PublishNamespaceResult> {
            co_return folly::makeUnexpected(PublishNamespaceError{
                ann.requestID,
                PublishNamespaceErrorCode::NOT_SUPPORTED,
                "nope"
            });
          }
      );

  PublishNamespace ann;
  ann.requestID = RequestID(2);
  auto result = folly::coro::blockingWait(filter_->publishNamespace(std::move(ann), nullptr));
  EXPECT_FALSE(result.hasValue());
  EXPECT_EQ(result.error().errorCode, PublishNamespaceErrorCode::NOT_SUPPORTED);
}

// ---- publish (sync, called directly) ----

TEST_F(SubscriberCrossExecFilterTest, PublishRunsOnTargetExec) {
  PublishOk ok;
  ok.requestID = RequestID(3);
  EXPECT_CALL(*inner_, publish(_, _))
      .WillOnce(
          [ok](PublishRequest, std::shared_ptr<SubscriptionHandle>) -> Subscriber::PublishResult {
            auto consumer = std::make_shared<NiceMock<MockTrackConsumer>>();
            return Subscriber::PublishConsumerAndReplyTask{
                std::move(consumer),
                folly::coro::makeTask<folly::Expected<PublishOk, PublishError>>(
                    folly::makeExpected<PublishError>(ok)
                )
            };
          }
      );

  PublishRequest pub;
  pub.requestID = RequestID(3);
  auto result = filter_->publish(std::move(pub), nullptr);
  ASSERT_TRUE(result.hasValue());
  // consumer is the cross-exec filter returned immediately
  EXPECT_NE(result.value().consumer, nullptr);
  // driving the reply task causes inner->publish() to run on targetExec_
  auto reply = folly::coro::blockingWait(std::move(result.value().reply));
  EXPECT_TRUE(reply.hasValue());
  EXPECT_EQ(reply.value().requestID, RequestID(3));
}

TEST_F(SubscriberCrossExecFilterTest, PublishInnerReturnsError) {
  EXPECT_CALL(*inner_, publish(_, _))
      .WillOnce(
          [](PublishRequest pub, std::shared_ptr<SubscriptionHandle>) -> Subscriber::PublishResult {
            return folly::makeUnexpected(
                PublishError{pub.requestID, PublishErrorCode::NOT_SUPPORTED, "nope"}
            );
          }
      );

  PublishRequest pub;
  pub.requestID = RequestID(4);
  auto result = filter_->publish(std::move(pub), nullptr);
  ASSERT_TRUE(result.hasValue());
  auto reply = folly::coro::blockingWait(std::move(result.value().reply));
  EXPECT_FALSE(reply.hasValue());
  EXPECT_EQ(reply.error().requestID, RequestID(4));
}

TEST_F(SubscriberCrossExecFilterTest, PublishDataFlowsThroughConsumer) {
  auto innerConsumer = std::make_shared<NiceMock<MockTrackConsumer>>();
  PublishOk ok;
  ok.requestID = RequestID(5);
  EXPECT_CALL(*inner_, publish(_, _))
      .WillOnce(
          [innerConsumer,
           ok](PublishRequest, std::shared_ptr<SubscriptionHandle>) -> Subscriber::PublishResult {
            return Subscriber::PublishConsumerAndReplyTask{
                innerConsumer,
                folly::coro::makeTask<folly::Expected<PublishOk, PublishError>>(
                    folly::makeExpected<PublishError>(ok)
                )
            };
          }
      );

  PublishRequest pub;
  pub.requestID = RequestID(5);
  auto result = filter_->publish(std::move(pub), nullptr);
  ASSERT_TRUE(result.hasValue());
  auto consumer = result.value().consumer;

  // Drive reply: calls inner->publish() on targetExec_ and wires up setDownstream().
  folly::coro::blockingWait(std::move(result.value().reply));

  // objectStream through consumer should reach innerConsumer via CrossExecFilter.
  std::promise<void> done;
  auto future = done.get_future();
  EXPECT_CALL(*innerConsumer, objectStream(_, _, _))
      .WillOnce([&done](const ObjectHeader&, Payload, bool) {
        done.set_value();
        return folly::makeExpected<MoQPublishError>(folly::unit);
      });
  consumer->objectStream(ObjectHeader{0, 0, 0}, nullptr, false);
  future.wait();
}

// ---- goaway ----

TEST_F(SubscriberCrossExecFilterTest, GoawayEnqueued) {
  folly::ManualExecutor manualExec;
  auto filter = std::make_shared<SubscriberCrossExecFilter>(&manualExec, inner_);

  EXPECT_CALL(*inner_, goaway(_)).Times(0);
  filter->goaway(Goaway{});

  EXPECT_CALL(*inner_, goaway(_)).Times(1);
  manualExec.drain();
}

} // namespace
