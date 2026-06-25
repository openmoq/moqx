/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Originally from github.com/facebookexperimental/moxygen.
 * See deps/moxygen/LICENSE for the original license terms.
 *
 * Copyright (c) OpenMOQ contributors.
 */

#include "MoqxRelayTestFixture.h"

namespace moxygen::test {

// Test: fetch fallback to subscriptions_ after publisher termination must not
// crash. When findPublishNamespaceSession returns null (no publishNamespace),
// fetch falls back to subscriptions_. After onPublishDone, upstream is null
// but the subscription entry remains if the forwarder has subscribers.
TEST_P(MoQRelayTest, FetchAfterPublisherTermination) {
  auto publisherSession = createMockSession();
  auto subSession = createMockSession();
  auto fetchSession = createMockSession();

  // Publish WITHOUT publishNamespace so findPublishNamespaceSession returns null
  // and fetch falls back to subscriptions_
  auto publishConsumer = doPublish(publisherSession, kTestTrackName, /*addToState=*/false);

  // Subscriber with open subgroup so subscription survives publisher drain
  auto consumer = createMockConsumer();
  auto sg = createMockSubgroupConsumer();
  EXPECT_CALL(*consumer, beginSubgroup(0, 0, _, _))
      .WillOnce([&sg](uint64_t, uint64_t, uint8_t, moxygen::BeginSubgroupOptions) {
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(sg);
      });
  auto handle = subscribeToTrack(subSession, kTestTrackName, consumer, RequestID(0));
  ASSERT_NE(handle, nullptr);

  // Begin subgroup to keep subscriber alive
  auto subgroupRes = publishConsumer->beginSubgroup(0, 0, 0);
  ASSERT_TRUE(subgroupRes.hasValue());

  // Publisher terminates — clears upstream but subscription stays
  EXPECT_CALL(*consumer, publishDone(_))
      .WillOnce(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
  publishConsumer->publishDone(
      {RequestID(0), PublishDoneStatusCode::SUBSCRIPTION_ENDED, 0, "publisher ended"}
  );

  // Fetch from a different session — falls back to subscriptions_, gets null
  // upstream, then crashes at line 1011 dereferencing null upstreamSession
  Fetch fetch(RequestID(0), kTestTrackName, AbsoluteLocation{0, 0}, AbsoluteLocation{1, 0});
  auto fetchConsumer = std::make_shared<NiceMock<MockFetchConsumer>>();
  withSessionContext(fetchSession, [&]() {
    auto task = publisherInterface()->fetch(std::move(fetch), fetchConsumer);
    auto res = folly::coro::blockingWait(std::move(task), exec_.get());
    // Should return an error, not crash
    EXPECT_FALSE(res.hasValue());
    EXPECT_EQ(res.error().errorCode, FetchErrorCode::DOES_NOT_EXIST);
  });

  // Clean up
  EXPECT_CALL(*sg, reset(_)).Times(1);
  subgroupRes.value()->reset(ResetStreamErrorCode::CANCELLED);
  handle->unsubscribe();
  removeSession(publisherSession);
  removeSession(subSession);
  removeSession(fetchSession);
}

// Joining fetch referencing a PUBLISH: onPublishOk must store the PUBLISH_OK
// request id so the fetch matches the fanned-out subscription and resolves.
TEST_P(MoQRelayTest, JoiningFetchAgainstPublish) {
  auto publisherSession = createMockSession();
  auto subscriber = createMockSession();

  doPublishNamespace(publisherSession, kTestNamespace);

  std::atomic<bool> published{false};
  auto mockConsumer = createMockConsumer();
  EXPECT_CALL(*subscriber, publish(_, _))
      .WillOnce([&mockConsumer, &published](const PublishRequest&, auto) {
        published.store(true);
        return Subscriber::PublishResult(Subscriber::PublishConsumerAndReplyTask{
            mockConsumer,
            []() -> folly::coro::Task<folly::Expected<PublishOk, PublishError>> {
              co_return PublishOk{
                  RequestID(7),
                  /*forward=*/true,
                  0,
                  GroupOrder::OldestFirst,
                  LocationType::LargestObject,
                  std::nullopt,
                  std::nullopt
              };
            }()
        });
      });

  doSubscribeNamespace(subscriber, kTestNamespace);

  // Publish the track with an initial largest so the fanned-out subscriber
  // snapshots a join point.
  PublishRequest pub;
  pub.fullTrackName = kTestTrackName;
  pub.largest = AbsoluteLocation{0, 0};
  withSessionContext(publisherSession, [&]() {
    auto res = subscriberInterface()->publish(std::move(pub), createMockSubscriptionHandle());
    ASSERT_TRUE(res.hasValue());
    getOrCreateMockState(publisherSession)->publishConsumers.push_back(res->consumer);
    co_withExecutor(static_cast<folly::DrivableExecutor*>(exec_.get()), std::move(res->reply))
        .start();
  });
  exec_->drive();
  ASSERT_TRUE(driveUntil([&] { return published.load(); }))
      << "publish was not forwarded to the subscriber";
  // Flush the PUBLISH_OK reply so onPublishOk stores the request id before the fetch.
  for (int i = 0; i < 5; ++i) {
    exec_->drive();
  }

  std::atomic<bool> upstreamFetched{false};
  auto capturedFetch = std::make_shared<Fetch>();
  EXPECT_CALL(*publisherSession, fetch(_, _))
      .WillOnce([capturedFetch, &upstreamFetched](Fetch f, std::shared_ptr<FetchConsumer>) {
        *capturedFetch = std::move(f);
        upstreamFetched.store(true);
        return folly::coro::makeTask<Publisher::FetchResult>(std::make_shared<MockFetchHandle>(
            FetchOk{RequestID(0), GroupOrder::OldestFirst, 0, AbsoluteLocation{0, 0}, {}}
        ));
      });

  Fetch joiningFetch(RequestID(0), RequestID(7), /*joiningStart=*/0, FetchType::RELATIVE_JOINING);
  joiningFetch.fullTrackName = kTestTrackName;
  auto fetchConsumer = std::make_shared<NiceMock<MockFetchConsumer>>();
  withSessionContext(subscriber, [&]() {
    auto task = publisherInterface()->fetch(std::move(joiningFetch), fetchConsumer);
    auto res = folly::coro::blockingWait(std::move(task), exec_.get());
    EXPECT_TRUE(res.hasValue());
  });
  ASSERT_TRUE(driveUntil([&] { return upstreamFetched.load(); }))
      << "joining fetch was not resolved/forwarded upstream";

  auto [standalone, joining] = fetchType(*capturedFetch);
  EXPECT_EQ(joining, nullptr) << "joining fetch was not resolved to standalone";
  ASSERT_NE(standalone, nullptr);
  EXPECT_EQ(standalone->start, (AbsoluteLocation{0, 0}));
  EXPECT_EQ(standalone->end, (AbsoluteLocation{0, 1}));

  removeSession(publisherSession);
  removeSession(subscriber);
  driveIfMultiThread();
}

} // namespace moxygen::test
