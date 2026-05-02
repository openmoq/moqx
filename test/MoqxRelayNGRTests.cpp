/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Originally from github.com/facebookexperimental/moxygen.
 * See deps/moxygen/LICENSE for the original license terms.
 *
 * Copyright (c) OpenMOQ contributors.
 */

#include "MoqxRelayTestFixture.h"

namespace moxygen::test {

// Test: relay PUBLISH path – dynamic groups from PublishRequest extensions
// is stored in the forwarder and forwarded to every downstream subscriber
TEST_F(MoQRelayTest, RelayPublishPropagatesDynamicGroupsToSubscribers) {
  auto publisherSession = createMockSession();
  auto subscriberSession = createMockSession();

  // Build a PublishRequest with DYNAMIC_GROUPS enabled
  PublishRequest pub;
  pub.fullTrackName = kTestTrackName;
  setPublisherDynamicGroups(pub, true);

  withSessionContext(publisherSession, [&]() {
    auto res = relay_->publish(std::move(pub), createMockSubscriptionHandle());
    ASSERT_TRUE(res.hasValue());
    getOrCreateMockState(publisherSession)->publishConsumers.push_back(res->consumer);
  });

  auto consumer = createMockConsumer();
  auto handle = subscribeToTrack(subscriberSession, kTestTrackName, consumer, RequestID(1));
  ASSERT_NE(handle, nullptr);

  auto dynGroups = getPublisherDynamicGroups(handle->subscribeOk());
  ASSERT_TRUE(dynGroups.has_value());
  EXPECT_TRUE(*dynGroups);

  removeSession(subscriberSession);
  exec_->drive();
  removeSession(publisherSession);
}

// Test: relay SUBSCRIBE path – dynamic groups from the upstream SubscribeOk is
// stored in the forwarder and forwarded to both the first and late-joining
// downstream subscribers
TEST_F(MoQRelayTest, RelaySubscribePropagatesDynamicGroupsToAllSubscribers) {
  auto publisherSession = createMockSession();
  auto subscriber1 = createMockSession();
  auto subscriber2 = createMockSession();

  doPublishNamespace(publisherSession, kTestNamespace);

  // Upstream returns a SubscribeOk with DYNAMIC_GROUPS = true
  SubscribeOk upstreamOk;
  upstreamOk.requestID = RequestID(1);
  upstreamOk.trackAlias = TrackAlias(1);
  upstreamOk.expires = std::chrono::milliseconds(0);
  upstreamOk.groupOrder = GroupOrder::OldestFirst;
  setPublisherDynamicGroups(upstreamOk, true);

  EXPECT_CALL(*publisherSession, subscribe(_, _))
      .WillOnce([upstreamOk](const auto& /*req*/, auto /*consumer*/) {
        auto handle = std::make_shared<NiceMock<MockSubscriptionHandle>>(upstreamOk);
        return folly::coro::makeTask<Publisher::SubscribeResult>(
            folly::Expected<std::shared_ptr<SubscriptionHandle>, SubscribeError>(handle)
        );
      });

  // First subscriber
  auto consumer1 = createMockConsumer();
  auto handle1 = subscribeToTrack(subscriber1, kTestTrackName, consumer1, RequestID(1));
  ASSERT_NE(handle1, nullptr);
  auto dynGroups1 = getPublisherDynamicGroups(handle1->subscribeOk());
  ASSERT_TRUE(dynGroups1.has_value());
  EXPECT_TRUE(*dynGroups1);

  // Late-joining second subscriber – forwarder should propagate the stored
  // dynamic groups value without another upstream roundtrip
  auto consumer2 = createMockConsumer();
  auto handle2 = subscribeToTrack(subscriber2, kTestTrackName, consumer2, RequestID(2));
  ASSERT_NE(handle2, nullptr);
  auto dynGroups2 = getPublisherDynamicGroups(handle2->subscribeOk());
  ASSERT_TRUE(dynGroups2.has_value());
  EXPECT_TRUE(*dynGroups2);

  removeSession(publisherSession);
  removeSession(subscriber1);
  removeSession(subscriber2);
}

// Relay test: When a late-joining subscriber sends NEW_GROUP_REQUEST in its
// SUBSCRIBE, the relay forwards it upstream via REQUEST_UPDATE
TEST_F(MoQRelayTest, RelaySubscribeLateJoinerNGRForwardedUpstream) {
  auto publisherSession = createMockSession();
  auto subscriber1 = createMockSession();
  auto subscriber2 = createMockSession();

  doPublishNamespace(publisherSession, kTestNamespace);

  // Upstream SubscribeOk advertises DYNAMIC_GROUPS = true
  SubscribeOk upstreamOk;
  upstreamOk.requestID = RequestID(100);
  upstreamOk.trackAlias = TrackAlias(1);
  upstreamOk.expires = std::chrono::milliseconds(0);
  upstreamOk.groupOrder = GroupOrder::OldestFirst;
  setPublisherDynamicGroups(upstreamOk, true);

  auto upstreamHandle = std::make_shared<NiceMock<MockSubscriptionHandle>>(upstreamOk);
  ON_CALL(*upstreamHandle, requestUpdateResult())
      .WillByDefault(Return(folly::makeExpected<RequestError>(
          RequestOk{RequestID(0), TrackRequestParameters(FrameType::REQUEST_OK), {}}
      )));

  EXPECT_CALL(*publisherSession, subscribe(_, _))
      .WillOnce([&upstreamHandle](const auto& /*req*/, auto /*consumer*/) {
        return folly::coro::makeTask<Publisher::SubscribeResult>(
            folly::Expected<std::shared_ptr<SubscriptionHandle>, SubscribeError>(upstreamHandle)
        );
      });

  // First subscriber establishes the upstream subscription (no NGR)
  auto consumer1 = createMockConsumer();
  auto handle1 = subscribeToTrack(subscriber1, kTestTrackName, consumer1, RequestID(1));
  ASSERT_NE(handle1, nullptr);

  // Second subscriber includes NEW_GROUP_REQUEST=8
  auto consumer2 = createMockConsumer();
  SubscribeRequest sub2;
  sub2.fullTrackName = kTestTrackName;
  sub2.requestID = RequestID(2);
  sub2.locType = LocationType::LargestObject;
  sub2.params.insertParam(
      Parameter(folly::to_underlying(TrackRequestParamKey::NEW_GROUP_REQUEST), uint64_t(8))
  );

  // The relay must forward NGR=8 upstream via REQUEST_UPDATE
  EXPECT_CALL(*upstreamHandle, requestUpdateCalled(_)).WillOnce([](const RequestUpdate& update) {
    auto ngrValue = getFirstIntParam(update.params, TrackRequestParamKey::NEW_GROUP_REQUEST);
    ASSERT_TRUE(ngrValue.has_value());
    EXPECT_EQ(*ngrValue, 8);
  });

  std::shared_ptr<SubscriptionHandle> handle2{nullptr};
  withSessionContext(subscriber2, [&]() {
    auto task = relay_->subscribe(std::move(sub2), consumer2);
    auto res = folly::coro::blockingWait(std::move(task), exec_.get());
    ASSERT_TRUE(res.hasValue());
    handle2 = *res;
  });
  exec_->drive();

  // Register handle2 so removeSession(subscriber2) will unsubscribe it,
  // allowing the forwarder to become empty and release the upstream handle.
  getOrCreateMockState(subscriber2)->subscribeHandles.push_back(handle2);

  removeSession(publisherSession);
  removeSession(subscriber1);
  removeSession(subscriber2);
  exec_->drive();
}

// Relay test: A downstream subscriber sending REQUEST_UPDATE with
// NEW_GROUP_REQUEST causes the relay to cascade the NGR upstream
TEST_F(MoQRelayTest, RelayRequestUpdateNGRCascadedUpstream) {
  auto publisherSession = createMockSession();
  auto subscriberSession = createMockSession();

  doPublishNamespace(publisherSession, kTestNamespace);

  // Upstream SubscribeOk advertises DYNAMIC_GROUPS = true
  SubscribeOk upstreamOk;
  upstreamOk.requestID = RequestID(100);
  upstreamOk.trackAlias = TrackAlias(1);
  upstreamOk.expires = std::chrono::milliseconds(0);
  upstreamOk.groupOrder = GroupOrder::OldestFirst;
  setPublisherDynamicGroups(upstreamOk, true);

  auto upstreamHandle = std::make_shared<NiceMock<MockSubscriptionHandle>>(upstreamOk);
  ON_CALL(*upstreamHandle, requestUpdateResult())
      .WillByDefault(Return(folly::makeExpected<RequestError>(
          RequestOk{RequestID(0), TrackRequestParameters(FrameType::REQUEST_OK), {}}
      )));

  EXPECT_CALL(*publisherSession, subscribe(_, _))
      .WillOnce([&upstreamHandle](const auto& /*req*/, auto /*consumer*/) {
        return folly::coro::makeTask<Publisher::SubscribeResult>(
            folly::Expected<std::shared_ptr<SubscriptionHandle>, SubscribeError>(upstreamHandle)
        );
      });

  // Subscribe downstream session - triggers upstream subscribe
  auto consumer = createMockConsumer();
  auto handle = subscribeToTrack(subscriberSession, kTestTrackName, consumer, RequestID(1));
  ASSERT_NE(handle, nullptr);

  auto* subscriber = dynamic_cast<MoQForwarder::Subscriber*>(handle.get());
  ASSERT_NE(subscriber, nullptr);

  // The relay must cascade NGR=9 upstream via REQUEST_UPDATE
  EXPECT_CALL(*upstreamHandle, requestUpdateCalled(_)).WillOnce([](const RequestUpdate& update) {
    auto ngrValue = getFirstIntParam(update.params, TrackRequestParamKey::NEW_GROUP_REQUEST);
    ASSERT_TRUE(ngrValue.has_value());
    EXPECT_EQ(*ngrValue, 9);
  });

  // Downstream subscriber sends REQUEST_UPDATE carrying NEW_GROUP_REQUEST=9
  RequestUpdate update;
  update.requestID = RequestID(2);
  update.existingRequestID = RequestID(1);
  update.params.insertParam(
      Parameter(folly::to_underlying(TrackRequestParamKey::NEW_GROUP_REQUEST), uint64_t(9))
  );
  folly::coro::blockingWait(subscriber->requestUpdate(std::move(update)));
  exec_->drive();

  removeSession(publisherSession);
  removeSession(subscriberSession);
}

} // namespace moxygen::test
