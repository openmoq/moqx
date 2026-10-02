/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/SubscriberCrossExecFilter.h"

#include <folly/CancellationToken.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/ViaIfAsync.h>
#include <folly/coro/WithCancellation.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/ManualExecutor.h>
#include <folly/io/async/EventBase.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <folly/synchronization/Baton.h>
#include <future>
#include <moxygen/test/Mocks.h>
#include <thread>

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

  // CrossExecFilter posts `delete this` to targetExec_, but ~CPUThreadPoolExecutor
  // abandons the queue and join() only drains what was queued before it. Barrier
  // until those deletes are posted (one hop of chain, plus slack), then join.
  void TearDown() override {
    filter_.reset();
    for (int i = 0; i < 2; ++i) {
      folly::Baton<> flushed;
      targetExec_->add([&flushed]() { flushed.post(); });
      flushed.wait();
    }
    targetExec_->join();
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

// The caller drops the returned handle on its own thread. The inner handle
// belongs to the inner session, so its last reference goes away on targetExec_.
TEST_F(SubscriberCrossExecFilterTest, PublishNamespaceHandleReleasesInnerOnTargetExec) {
  std::thread::id targetThread;
  std::thread::id destroyedOn;
  EXPECT_CALL(*inner_, publishNamespace(_, _))
      .WillOnce(
          [&targetThread,
           &destroyedOn](PublishNamespace, std::shared_ptr<Subscriber::PublishNamespaceCallback>)
              -> folly::coro::Task<Subscriber::PublishNamespaceResult> {
            targetThread = std::this_thread::get_id();
            co_return std::shared_ptr<Subscriber::PublishNamespaceHandle>(
                new NiceMock<MockPublishNamespaceHandle>(),
                [&destroyedOn](Subscriber::PublishNamespaceHandle* h) {
                  destroyedOn = std::this_thread::get_id();
                  delete h;
                }
            );
          }
      );

  PublishNamespace ann;
  ann.requestID = RequestID(8);
  auto result = folly::coro::blockingWait(filter_->publishNamespace(std::move(ann), nullptr));
  ASSERT_TRUE(result.hasValue());
  result.value().reset();

  folly::Baton<> flushed;
  targetExec_->add([&flushed]() { flushed.post(); });
  flushed.wait();
  EXPECT_EQ(destroyedOn, targetThread);
}

// The inner session drops the callback wrapper on targetExec_. The caller's
// callback belongs to the caller, so its last reference goes away on callerExec.
TEST_F(SubscriberCrossExecFilterTest, PublishNamespaceCallbackReleasesInnerOnCallerExec) {
  folly::ManualExecutor callerExec;
  std::shared_ptr<Subscriber::PublishNamespaceCallback> captured;
  EXPECT_CALL(*inner_, publishNamespace(_, _))
      .WillOnce(
          [&captured](
              PublishNamespace ann,
              std::shared_ptr<Subscriber::PublishNamespaceCallback> cb
          ) -> folly::coro::Task<Subscriber::PublishNamespaceResult> {
            captured = std::move(cb);
            co_return folly::makeUnexpected(PublishNamespaceError{
                ann.requestID,
                PublishNamespaceErrorCode::NOT_SUPPORTED,
                "nope"
            });
          }
      );

  std::thread::id destroyedOn;
  std::shared_ptr<Subscriber::PublishNamespaceCallback> callback(
      new NiceMock<MockPublishNamespaceCallback>(),
      [&destroyedOn](Subscriber::PublishNamespaceCallback* cb) {
        destroyedOn = std::this_thread::get_id();
        delete cb;
      }
  );
  PublishNamespace ann;
  ann.requestID = RequestID(9);
  auto fut = folly::coro::co_withExecutor(
                 &callerExec,
                 filter_->publishNamespace(std::move(ann), std::move(callback))
  )
                 .start();
  while (!fut.isReady()) {
    callerExec.drive();
  }

  folly::Baton<> dropped;
  targetExec_->add([&captured, &dropped]() {
    captured.reset();
    dropped.post();
  });
  dropped.wait();
  callerExec.drain();
  EXPECT_EQ(destroyedOn, std::this_thread::get_id());
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

// A closing session cancels, then drops the filter, while publishNamespace is
// still awaiting the inner subscriber. The coroutine holds only a raw this, so
// ASan catches any member read after the await. The cancelled call must still
// destroy the inner handle on targetExec_.
TEST_F(SubscriberCrossExecFilterTest, PublishNamespaceResumesAfterFilterDestroyed) {
  folly::EventBase evb;
  folly::Baton<> innerEntered;
  folly::Baton<> innerRelease;
  folly::CancellationSource cancelSource;
  std::thread::id targetThread;
  std::thread::id destroyedOn;
  EXPECT_CALL(*inner_, publishNamespace(_, _))
      .WillOnce(
          [&evb,
           &innerEntered,
           &innerRelease,
           &targetThread,
           &destroyedOn](PublishNamespace, std::shared_ptr<Subscriber::PublishNamespaceCallback>)
              -> folly::coro::Task<Subscriber::PublishNamespaceResult> {
            targetThread = std::this_thread::get_id();
            std::shared_ptr<Subscriber::PublishNamespaceHandle> handle(
                new NiceMock<MockPublishNamespaceHandle>(),
                [&destroyedOn](Subscriber::PublishNamespaceHandle* h) {
                  destroyedOn = std::this_thread::get_id();
                  delete h;
                }
            );
            // Wakes the loop below, so it never blocks on an empty EventBase.
            evb.runInEventBaseThread([&innerEntered]() { innerEntered.post(); });
            innerRelease.wait();
            co_return handle;
          }
      );

  PublishNamespace ann;
  ann.requestID = RequestID(7);
  auto fut = folly::coro::co_withExecutor(
                 folly::getKeepAliveToken(&evb),
                 folly::coro::co_withCancellation(
                     cancelSource.getToken(),
                     filter_->publishNamespace(std::move(ann), nullptr)
                 )
  )
                 .start();

  while (!innerEntered.ready()) {
    evb.loopOnce();
  }

  // MoQSession::cleanup's order: request cancellation, then release the filter.
  cancelSource.requestCancellation();
  filter_.reset();
  innerRelease.post();

  EXPECT_THROW(std::move(fut).via(&evb).getVia(&evb), folly::OperationCancelled);

  folly::Baton<> flushed;
  targetExec_->add([&flushed]() { flushed.post(); });
  flushed.wait();
  EXPECT_EQ(destroyedOn, targetThread);
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

TEST_F(SubscriberCrossExecFilterTest, PublishSafeAfterFilterDestroyed) {
  // Regression: the old coPublish member coroutine captured `this` implicitly.
  // Destroying the filter before driving the reply task was a use-after-free.
  // The fix captures exec and inner by value in co_invoke so `this` is not needed.
  PublishOk ok;
  ok.requestID = RequestID(6);
  EXPECT_CALL(*inner_, publish(_, _))
      .WillOnce(
          [ok](PublishRequest, std::shared_ptr<SubscriptionHandle>) -> Subscriber::PublishResult {
            return Subscriber::PublishConsumerAndReplyTask{
                std::make_shared<NiceMock<MockTrackConsumer>>(),
                folly::coro::makeTask<folly::Expected<PublishOk, PublishError>>(
                    folly::makeExpected<PublishError>(ok)
                )
            };
          }
      );

  // Obtain the reply task while the filter is alive on the stack, then let it die.
  auto replyTask = [&]() {
    SubscriberCrossExecFilter stackFilter(targetExec_.get(), inner_);
    PublishRequest pub;
    pub.requestID = RequestID(6);
    auto result = stackFilter.publish(std::move(pub), nullptr);
    EXPECT_TRUE(result.hasValue());
    return std::move(result.value().reply);
  }();

  // stackFilter is destroyed; driving the task must not access a dangling this.
  auto reply = folly::coro::blockingWait(std::move(replyTask));
  EXPECT_TRUE(reply.hasValue());
  EXPECT_EQ(reply.value().requestID, RequestID(6));
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
