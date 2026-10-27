/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Copyright (c) OpenMOQ contributors.
 * Originally from github.com/facebookexperimental/moxygen.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MoqxRelayTestFixture.h"

#include <folly/ScopeGuard.h>
#include <folly/coro/Baton.h>
#include <folly/futures/Promise.h>

#include <deque>
#include <mutex>

namespace openmoq::moqx::test {

namespace {
// Fails any upstream fetch, and records that one was made.
void failUpstreamFetches(MockMoQSession& session, std::atomic<bool>& fetched) {
  ON_CALL(session, fetch(_, _)).WillByDefault([&fetched](Fetch, std::shared_ptr<FetchConsumer>) {
    fetched.store(true);
    return folly::coro::makeTask<Publisher::FetchResult>(folly::makeUnexpected(
        FetchError{RequestID(0), FetchErrorCode::INTERNAL_ERROR, "unexpected upstream fetch"}
    ));
  });
}

// Answers the upstream SUBSCRIBE with `largest`.
void answerUpstreamSubscribe(MockMoQSession& session, AbsoluteLocation largest) {
  SubscribeOk ok;
  ok.requestID = RequestID(1);
  ok.trackAlias = TrackAlias(1);
  ok.expires = std::chrono::milliseconds(0);
  ok.groupOrder = GroupOrder::OldestFirst;
  ok.largest = largest;
  EXPECT_CALL(session, subscribe(_, _)).WillOnce([ok](const auto& /*req*/, auto /*consumer*/) {
    return folly::coro::makeTask<Publisher::SubscribeResult>(
        folly::Expected<std::shared_ptr<SubscriptionHandle>, SubscribeError>(
            std::make_shared<NiceMock<MockSubscriptionHandle>>(ok)
        )
    );
  });
}

// Records the upstream FETCH into `captured` and ends it at once.
void answerUpstreamFetch(MockMoQSession& session, std::shared_ptr<Fetch> captured) {
  EXPECT_CALL(session, fetch(_, _))
      .WillOnce([captured](Fetch f, std::shared_ptr<FetchConsumer> consumer) {
        *captured = std::move(f);
        consumer->endOfFetch();
        return folly::coro::makeTask<Publisher::FetchResult>(std::make_shared<MockFetchHandle>(
            FetchOk{RequestID(0), GroupOrder::OldestFirst, 0, AbsoluteLocation{5, 0}, {}}
        ));
      });
}

// Accepts one fill for `requestID` on `consumer` and sets `ended` when the fill ends.
void acceptFill(MockTrackConsumer& consumer, RequestID requestID, std::atomic<bool>& ended) {
  auto fill = std::make_shared<NiceMock<MockFetchConsumer>>();
  EXPECT_CALL(*fill, endOfFetch()).WillOnce([&ended]() {
    ended.store(true);
    return folly::makeExpected<MoQPublishError>(folly::unit);
  });
  EXPECT_CALL(consumer, beginFill(requestID))
      .WillOnce(Return(
          folly::makeExpected<MoQPublishError>(std::shared_ptr<FetchConsumer>(std::move(fill)))
      ));
}

FillParameters fillFrom(AbsoluteLocation start) {
  FillParameters fill;
  fill.locationFilter = LocationFilter::absoluteStart(start);
  return fill;
}
} // namespace

// Test: FETCH with an empty namespace is rejected pre-draft-18.
TEST_P(MoQRelayTest, FetchEmptyNamespaceRejectedPreV18) {
  auto session = createMockSession();
  // Default session negotiates kVersionDraft16 (< 18)

  Fetch fetch(
      RequestID(0),
      FullTrackName{TrackNamespace{{}}, "track1"},
      AbsoluteLocation{0, 0},
      AbsoluteLocation{1, 0}
  );
  auto fetchConsumer = std::make_shared<NiceMock<MockFetchConsumer>>();
  withSessionContext(session, [&]() {
    auto task = publisherInterface()->fetch(std::move(fetch), fetchConsumer);
    auto res = folly::coro::blockingWait(std::move(task), exec_.get());
    ASSERT_FALSE(res.hasValue());
    EXPECT_EQ(res.error().errorCode, FetchErrorCode::DOES_NOT_EXIST);
    EXPECT_EQ(res.error().reasonPhrase, "namespace required");
  });

  removeSession(session);
}

// Test: FETCH with an empty namespace is not rejected for its namespace on
// draft-18+ — it falls through to the normal "no such upstream" resolution.
TEST_P(MoQRelayTest, FetchEmptyNamespaceAllowedV18) {
  auto session = createMockSession();
  ON_CALL(*session, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft18)));

  Fetch fetch(
      RequestID(0),
      FullTrackName{TrackNamespace{{}}, "track1"},
      AbsoluteLocation{0, 0},
      AbsoluteLocation{1, 0}
  );
  auto fetchConsumer = std::make_shared<NiceMock<MockFetchConsumer>>();
  withSessionContext(session, [&]() {
    auto task = publisherInterface()->fetch(std::move(fetch), fetchConsumer);
    auto res = folly::coro::blockingWait(std::move(task), exec_.get());
    ASSERT_FALSE(res.hasValue());
    EXPECT_EQ(res.error().errorCode, FetchErrorCode::DOES_NOT_EXIST);
    EXPECT_EQ(res.error().reasonPhrase, "no upstream for fetch");
  });

  removeSession(session);
}

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

// Test: FETCH must prefer an exact-track upstream over a broader
// namespace-level publisher when both exist for the same track.
TEST_P(MoQRelayTest, FetchPrefersExactTrackOverNamespace) {
  auto namespacePublisher = createMockSession();
  auto trackPublisher = createMockSession();
  auto fetchSession = createMockSession();

  // Broad, namespace-level publisher for kTestNamespace.
  doPublishNamespace(namespacePublisher, kTestNamespace);

  // A distinct, more specific publisher for the exact track, landing in the
  // registry rather than the namespace tree.
  doPublish(trackPublisher, kTestTrackName, /*addToState=*/true);

  EXPECT_CALL(*namespacePublisher, fetch(_, _)).Times(0);
  EXPECT_CALL(*trackPublisher, fetch(_, _))
      .WillOnce([](Fetch, std::shared_ptr<FetchConsumer> consumer) {
        // Terminate the fetch so the cross-exec consumer drops its
        // self-anchor; a real upstream always ends the fetch.
        consumer->endOfFetch();
        return folly::coro::makeTask<Publisher::FetchResult>(std::make_shared<MockFetchHandle>(
            FetchOk{RequestID(0), GroupOrder::OldestFirst, 0, AbsoluteLocation{0, 0}, {}}
        ));
      });

  Fetch fetch(RequestID(0), kTestTrackName, AbsoluteLocation{0, 0}, AbsoluteLocation{1, 0});
  auto fetchConsumer = std::make_shared<NiceMock<MockFetchConsumer>>();
  EXPECT_CALL(*fetchConsumer, endOfFetch()).WillOnce([]() {
    return folly::Expected<folly::Unit, MoQPublishError>(folly::unit);
  });
  withSessionContext(fetchSession, [&]() {
    auto task = publisherInterface()->fetch(std::move(fetch), fetchConsumer);
    auto res = folly::coro::blockingWait(std::move(task), exec_.get());
    EXPECT_TRUE(res.hasValue());
  });

  removeSession(namespacePublisher);
  removeSession(trackPublisher);
  removeSession(fetchSession);
}

// A republished track's PUBLISH Largest is past the cached objects, so a FETCH
// up to it goes upstream for the rest.
TEST_P(MoQRelayTest, FetchAfterRepublishRunsToPublishLargest) {
  resetRelay(config::CacheConfig{.maxCachedTracks = 10});
  relay_->setAllowedNamespacePrefix(kAllowedPrefix);
  auto firstPublisher = createMockSession();
  auto secondPublisher = createMockSession();
  auto fetchSession = createMockSession();

  auto firstConsumer = doPublish(firstPublisher, kTestTrackName, /*addToState=*/false);
  auto sg = firstConsumer->beginSubgroup(0, 0, 0);
  ASSERT_TRUE(sg.hasValue()) << sg.error().describe();
  EXPECT_TRUE(sg.value()->object(0, folly::IOBuf::copyBuffer("abc")).hasValue());
  EXPECT_TRUE(sg.value()->endOfSubgroup().hasValue());
  firstConsumer->publishDone(
      {RequestID(0), PublishDoneStatusCode::SUBSCRIPTION_ENDED, 0, "publisher ended"}
  );
  driveIfMultiThread();

  PublishRequest pub;
  pub.fullTrackName = kTestTrackName;
  pub.largest = AbsoluteLocation{5, 0};
  withSessionContext(secondPublisher, [&]() {
    auto res = subscriberInterface()->publish(std::move(pub), createMockSubscriptionHandle());
    ASSERT_TRUE(res.hasValue());
    getOrCreateMockState(secondPublisher)->publishConsumers.push_back(res->consumer);
    co_withExecutor(static_cast<folly::DrivableExecutor*>(exec_.get()), std::move(res->reply))
        .start();
  });
  driveIfMultiThread();

  std::atomic<bool> upstreamFetched{false};
  auto capturedFetch = std::make_shared<Fetch>();
  EXPECT_CALL(*secondPublisher, fetch(_, _))
      .WillOnce([capturedFetch,
                 &upstreamFetched](Fetch f, std::shared_ptr<FetchConsumer> consumer) {
        *capturedFetch = std::move(f);
        upstreamFetched.store(true);
        consumer->endOfFetch();
        return folly::coro::makeTask<Publisher::FetchResult>(std::make_shared<MockFetchHandle>(
            FetchOk{RequestID(0), GroupOrder::OldestFirst, 0, AbsoluteLocation{5, 1}, {}}
        ));
      });

  Fetch fetch(RequestID(0), kTestTrackName, AbsoluteLocation{0, 0}, AbsoluteLocation{6, 0});
  auto fetchConsumer = std::make_shared<NiceMock<MockFetchConsumer>>();
  EXPECT_CALL(*fetchConsumer, object(0, _, 0, _, _, _, _))
      .WillOnce(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
  ON_CALL(*fetchConsumer, endOfFetch())
      .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
  withSessionContext(fetchSession, [&]() {
    auto task = publisherInterface()->fetch(std::move(fetch), fetchConsumer);
    auto res = folly::coro::blockingWait(std::move(task), exec_.get());
    ASSERT_TRUE(res.hasValue());
    EXPECT_EQ(res.value()->fetchOk().endLocation, (AbsoluteLocation{5, 1}));
  });
  ASSERT_TRUE(driveUntil([&] { return upstreamFetched.load(); }))
      << "fetch past the cached Largest did not go upstream";
  auto [standalone, joining] = fetchType(*capturedFetch);
  ASSERT_NE(standalone, nullptr);
  EXPECT_EQ(standalone->start, (AbsoluteLocation{0, 1}));

  removeSession(firstPublisher);
  removeSession(secondPublisher);
  removeSession(fetchSession);
  driveIfMultiThread();
}

// A subscriber pauses and resumes a PUBLISH. The resume's OK reports the Largest from
// the publisher's REQUEST_UPDATE_OK, and a FETCH for the gap goes upstream.
TEST_P(MoQRelayTest, ResumeReportsUpstreamLargest) {
  resetRelay(config::CacheConfig{.maxCachedTracks = 10});
  relay_->setAllowedNamespacePrefix(kAllowedPrefix);
  auto publisherSession = createMockSession();
  auto subscriberSession = createMockSession();
  auto fetchSession = createMockSession();

  // The publisher reports upstreamLargest in each REQUEST_UPDATE_OK.
  auto upstreamLargest = std::make_shared<AbsoluteLocation>(AbsoluteLocation{3, 0});
  auto mockHandle = makePublishHandle();
  ON_CALL(*mockHandle, requestUpdateResult()).WillByDefault([upstreamLargest] {
    RequestOk ok;
    ok.requestSpecificParams.emplace_back(
        folly::to_underlying(TrackRequestParamKey::LARGEST_OBJECT),
        *upstreamLargest
    );
    return folly::makeExpected<RequestError>(std::move(ok));
  });
  auto publishConsumer = doPublishWithHandle(publisherSession, kTestTrackName, mockHandle);
  auto sg = publishConsumer->beginSubgroup(0, 0, 0);
  ASSERT_TRUE(sg.hasValue()) << sg.error().describe();
  EXPECT_TRUE(sg.value()->object(0, folly::IOBuf::copyBuffer("abc")).hasValue());
  EXPECT_TRUE(sg.value()->endOfSubgroup().hasValue());
  driveIfMultiThread();

  auto handle =
      subscribeToTrack(subscriberSession, kTestTrackName, createMockConsumer(), RequestID(1));
  ASSERT_NE(handle, nullptr);
  auto forwardUpdate = [&](RequestID requestID, bool forward) {
    RequestUpdate update;
    update.requestID = requestID;
    update.existingRequestID = RequestID(1);
    update.forward = forward;
    return folly::coro::blockingWait(handle->requestUpdate(std::move(update)), exec_.get());
  };
  ASSERT_TRUE(forwardUpdate(RequestID(2), false).hasValue());
  driveIfMultiThread();

  // The publisher produced through {5, 0} while paused.
  *upstreamLargest = AbsoluteLocation{5, 0};
  auto ok = forwardUpdate(RequestID(3), true);
  ASSERT_TRUE(ok.hasValue());
  ASSERT_EQ(ok->requestSpecificParams.size(), 1);
  EXPECT_EQ(ok->requestSpecificParams[0].largestObject, (AbsoluteLocation{5, 0}));

  std::atomic<bool> upstreamFetched{false};
  auto capturedFetch = std::make_shared<Fetch>();
  EXPECT_CALL(*publisherSession, fetch(_, _))
      .WillOnce([capturedFetch,
                 &upstreamFetched](Fetch f, std::shared_ptr<FetchConsumer> consumer) {
        *capturedFetch = std::move(f);
        upstreamFetched.store(true);
        consumer->endOfFetch();
        return folly::coro::makeTask<Publisher::FetchResult>(std::make_shared<MockFetchHandle>(
            FetchOk{RequestID(0), GroupOrder::OldestFirst, 0, AbsoluteLocation{5, 1}, {}}
        ));
      });
  Fetch fetch(RequestID(0), kTestTrackName, AbsoluteLocation{0, 0}, AbsoluteLocation{6, 0});
  auto fetchConsumer = std::make_shared<NiceMock<MockFetchConsumer>>();
  EXPECT_CALL(*fetchConsumer, object(0, _, 0, _, _, _, _))
      .WillOnce(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
  ON_CALL(*fetchConsumer, endOfFetch())
      .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
  withSessionContext(fetchSession, [&]() {
    auto task = publisherInterface()->fetch(std::move(fetch), fetchConsumer);
    auto res = folly::coro::blockingWait(std::move(task), exec_.get());
    ASSERT_TRUE(res.hasValue());
    EXPECT_EQ(res.value()->fetchOk().endLocation, (AbsoluteLocation{5, 1}));
  });
  ASSERT_TRUE(driveUntil([&] { return upstreamFetched.load(); }))
      << "fetch past the cached Largest did not go upstream";
  auto [standalone, joining] = fetchType(*capturedFetch);
  ASSERT_NE(standalone, nullptr);
  EXPECT_EQ(standalone->start, (AbsoluteLocation{0, 1}));

  handle->unsubscribe();
  removeSession(subscriberSession);
  removeSession(fetchSession);
  removeSession(publisherSession);
  driveIfMultiThread();
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
                  LocationFilter::nextObject()
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
      .WillOnce([capturedFetch,
                 &upstreamFetched](Fetch f, std::shared_ptr<FetchConsumer> consumer) {
        *capturedFetch = std::move(f);
        upstreamFetched.store(true);
        // Terminate the fetch so the cross-exec consumer drops its self-anchor;
        // a real upstream always ends the fetch (here the resolved range is empty).
        consumer->endOfFetch();
        return folly::coro::makeTask<Publisher::FetchResult>(std::make_shared<MockFetchHandle>(
            FetchOk{RequestID(0), GroupOrder::OldestFirst, 0, AbsoluteLocation{0, 0}, {}}
        ));
      });

  Fetch joiningFetch(RequestID(0), RequestID(7), /*joiningStart=*/0, FetchType::RELATIVE_JOINING);
  joiningFetch.fullTrackName = kTestTrackName;
  auto fetchConsumer = std::make_shared<NiceMock<MockFetchConsumer>>();
  EXPECT_CALL(*fetchConsumer, endOfFetch()).WillOnce([]() {
    return folly::Expected<folly::Unit, MoQPublishError>(folly::unit);
  });
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

// A joining FETCH pipelined behind its SUBSCRIBE, sent while that subscribe is still setting up
// this thread's forwarder, resolves against the joiner's subscription once setup completes.
TEST_P(MoQRelayTest, JoiningFetchWaitsForItsSubscribeSetup) {
  if (relayMode() != RelayMode::LocalForwarderMT) {
    GTEST_SKIP() << "only LF sets up a per-thread forwarder the fetch can race";
  }

  // Publisher on its own iothread, so the subscriber thread's entry stays Pending while the
  // upstream SUBSCRIBE is held.
  auto& pubAux = makeAuxExec("pub-iothread");
  auto* pubEvb = pubAux.evb;
  auto publisherSession = std::make_shared<NiceMock<MockMoQSession>>(pubAux.exec);
  ON_CALL(*publisherSession, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft16)));
  getOrCreateMockState(publisherSession);
  auto subSession = createMockSession();
  doPublishNamespace(publisherSession, kTestNamespace);

  SubscribeOk upstreamOk;
  upstreamOk.requestID = RequestID(1);
  upstreamOk.trackAlias = TrackAlias(1);
  upstreamOk.expires = std::chrono::milliseconds(0);
  upstreamOk.groupOrder = GroupOrder::OldestFirst;
  upstreamOk.largest = AbsoluteLocation{3, 2};

  folly::coro::Baton upstreamGate;
  std::atomic<bool> upstreamSubscribeCalled{false};
  EXPECT_CALL(*publisherSession, subscribe(_, _))
      .WillOnce(
          [&](const SubscribeRequest&,
              std::shared_ptr<TrackConsumer>) -> folly::coro::Task<Publisher::SubscribeResult> {
            upstreamSubscribeCalled.store(true);
            co_await upstreamGate;
            auto handle = std::make_shared<NiceMock<MockSubscriptionHandle>>(upstreamOk);
            co_return folly::Expected<std::shared_ptr<SubscriptionHandle>, SubscribeError>(handle);
          }
      );
  auto releaseGate = folly::makeGuard([&]() noexcept { upstreamGate.post(); });

  std::atomic<bool> upstreamFetched{false};
  auto capturedFetch = std::make_shared<Fetch>();
  EXPECT_CALL(*publisherSession, fetch(_, _))
      .WillOnce([capturedFetch,
                 &upstreamFetched](Fetch f, std::shared_ptr<FetchConsumer> consumer) {
        *capturedFetch = std::move(f);
        upstreamFetched.store(true);
        consumer->endOfFetch();
        return folly::coro::makeTask<Publisher::FetchResult>(std::make_shared<MockFetchHandle>(
            FetchOk{RequestID(0), GroupOrder::OldestFirst, 0, AbsoluteLocation{3, 3}, {}}
        ));
      });

  auto pump = [&](auto pred) {
    for (int i = 0; i < 1000 && !pred(); ++i) {
      exec_->drive();
      pubEvb->runInEventBaseThreadAndWait([] {});
    }
    return pred();
  };

  auto subResult = std::make_shared<std::optional<Publisher::SubscribeResult>>();
  std::atomic<bool> subscribeDone{false};
  auto fetchResult = std::make_shared<std::optional<Publisher::FetchResult>>();
  std::atomic<bool> fetchDone{false};
  auto fetchConsumer = std::make_shared<NiceMock<MockFetchConsumer>>();
  EXPECT_CALL(*fetchConsumer, endOfFetch()).WillOnce([]() {
    return folly::Expected<folly::Unit, MoQPublishError>(folly::unit);
  });
  withSessionContext(subSession, [&]() {
    SubscribeRequest sub;
    sub.fullTrackName = kTestTrackName;
    sub.requestID = RequestID(0);
    sub.filter = LocationFilter::nextObject();
    auto subTask = publisherInterface()->subscribe(std::move(sub), createMockConsumer());
    co_withExecutor(
        static_cast<folly::DrivableExecutor*>(exec_.get()),
        folly::coro::co_invoke(
            [t = std::move(subTask), subResult, &subscribeDone](
            ) mutable -> folly::coro::Task<void> {
              *subResult = co_await std::move(t);
              subscribeDone.store(true);
            }
        )
    ).start();
  });
  ASSERT_TRUE(pump([&] { return upstreamSubscribeCalled.load(); }))
      << "subscribe should park in the gated upstream SUBSCRIBE";

  withSessionContext(subSession, [&]() {
    Fetch joining(RequestID(2), RequestID(0), /*joiningStart=*/1, FetchType::RELATIVE_JOINING);
    joining.fullTrackName = kTestTrackName;
    auto fetchTask = publisherInterface()->fetch(std::move(joining), fetchConsumer);
    co_withExecutor(
        static_cast<folly::DrivableExecutor*>(exec_.get()),
        folly::coro::co_invoke(
            [t = std::move(fetchTask), fetchResult, &fetchDone](
            ) mutable -> folly::coro::Task<void> {
              *fetchResult = co_await std::move(t);
              fetchDone.store(true);
            }
        )
    ).start();
  });
  for (int i = 0; i < 50; ++i) {
    exec_->drive();
    pubEvb->runInEventBaseThreadAndWait([] {});
  }
  EXPECT_FALSE(upstreamFetched.load()) << "the joining fetch went upstream before its subscribe";

  releaseGate.dismiss();
  upstreamGate.post();
  ASSERT_TRUE(pump([&] { return subscribeDone.load() && fetchDone.load(); }));
  ASSERT_TRUE(subResult->value().hasValue());
  EXPECT_TRUE(fetchResult->value().hasValue());
  ASSERT_TRUE(upstreamFetched.load());

  // One group back from Largest {3, 2}.
  auto [standalone, joiningArgs] = fetchType(*capturedFetch);
  EXPECT_EQ(joiningArgs, nullptr) << "joining fetch was deferred instead of resolved";
  ASSERT_NE(standalone, nullptr);
  EXPECT_EQ(standalone->start, (AbsoluteLocation{2, 0}));
  EXPECT_EQ(standalone->end, (AbsoluteLocation{3, 3}));

  subResult->value().value()->unsubscribe();
  subResult->reset();
  fetchResult->reset();
  removeSession(publisherSession);
  removeSession(subSession);
  for (int i = 0; i < 4; ++i) {
    driveIfMultiThread();
    pubEvb->runInEventBaseThreadAndWait([] {});
  }
}

// A joining FETCH pipelined behind a SUBSCRIBE that fails gets an error from the relay.
TEST_P(MoQRelayTest, JoiningFetchFailsWithItsSubscribe) {
  if (relayMode() != RelayMode::LocalForwarderMT) {
    GTEST_SKIP() << "only LF sets up a per-thread forwarder the fetch can race";
  }

  auto& pubAux = makeAuxExec("pub-iothread");
  auto* pubEvb = pubAux.evb;
  auto publisherSession = std::make_shared<NiceMock<MockMoQSession>>(pubAux.exec);
  ON_CALL(*publisherSession, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft16)));
  getOrCreateMockState(publisherSession);
  auto subSession = createMockSession();
  doPublishNamespace(publisherSession, kTestNamespace);

  folly::coro::Baton upstreamGate;
  std::atomic<bool> upstreamSubscribeCalled{false};
  EXPECT_CALL(*publisherSession, subscribe(_, _))
      .WillOnce(
          [&](const SubscribeRequest&,
              std::shared_ptr<TrackConsumer>) -> folly::coro::Task<Publisher::SubscribeResult> {
            upstreamSubscribeCalled.store(true);
            co_await upstreamGate;
            co_return folly::makeUnexpected(
                SubscribeError{RequestID(1), SubscribeErrorCode::INTERNAL_ERROR, "upstream failed"}
            );
          }
      );
  auto releaseGate = folly::makeGuard([&]() noexcept { upstreamGate.post(); });
  std::atomic<bool> upstreamFetched{false};
  failUpstreamFetches(*publisherSession, upstreamFetched);

  auto pump = [&](auto pred) {
    for (int i = 0; i < 1000 && !pred(); ++i) {
      exec_->drive();
      pubEvb->runInEventBaseThreadAndWait([] {});
    }
    return pred();
  };

  auto subResult = std::make_shared<std::optional<Publisher::SubscribeResult>>();
  std::atomic<bool> subscribeDone{false};
  auto fetchResult = std::make_shared<std::optional<Publisher::FetchResult>>();
  std::atomic<bool> fetchDone{false};
  auto fetchConsumer = std::make_shared<NiceMock<MockFetchConsumer>>();
  withSessionContext(subSession, [&]() {
    auto subTask =
        publisherInterface()->subscribe(makeSubscribeRequest(RequestID(0)), createMockConsumer());
    co_withExecutor(
        static_cast<folly::DrivableExecutor*>(exec_.get()),
        folly::coro::co_invoke(
            [t = std::move(subTask), subResult, &subscribeDone](
            ) mutable -> folly::coro::Task<void> {
              *subResult = co_await std::move(t);
              subscribeDone.store(true);
            }
        )
    ).start();
  });
  ASSERT_TRUE(pump([&] { return upstreamSubscribeCalled.load(); }))
      << "subscribe should wait in the gated upstream SUBSCRIBE";

  withSessionContext(subSession, [&]() {
    Fetch joining(RequestID(2), RequestID(0), /*joiningStart=*/1, FetchType::RELATIVE_JOINING);
    joining.fullTrackName = kTestTrackName;
    auto fetchTask = publisherInterface()->fetch(std::move(joining), fetchConsumer);
    co_withExecutor(
        static_cast<folly::DrivableExecutor*>(exec_.get()),
        folly::coro::co_invoke(
            [t = std::move(fetchTask), fetchResult, &fetchDone](
            ) mutable -> folly::coro::Task<void> {
              *fetchResult = co_await std::move(t);
              fetchDone.store(true);
            }
        )
    ).start();
  });

  releaseGate.dismiss();
  upstreamGate.post();
  ASSERT_TRUE(pump([&] { return subscribeDone.load() && fetchDone.load(); }));
  EXPECT_FALSE(subResult->value().hasValue());
  EXPECT_FALSE(fetchResult->value().hasValue());
  EXPECT_FALSE(upstreamFetched.load()) << "the joining fetch went upstream unresolved";

  subResult->reset();
  fetchResult->reset();
  removeSession(publisherSession);
  removeSession(subSession);
  for (int i = 0; i < 4; ++i) {
    driveIfMultiThread();
    pubEvb->runInEventBaseThreadAndWait([] {});
  }
}

// A joining FETCH that names a subscription the session does not have gets an error from the
// relay.
TEST_P(MoQRelayTest, JoiningFetchWithoutSubscriptionFails) {
  auto publisherSession = createMockSession();
  auto fetchSession = createMockSession();
  doPublishNamespace(publisherSession, kTestNamespace);
  std::atomic<bool> upstreamFetched{false};
  failUpstreamFetches(*publisherSession, upstreamFetched);

  Fetch joining(RequestID(2), RequestID(0), /*joiningStart=*/1, FetchType::RELATIVE_JOINING);
  joining.fullTrackName = kTestTrackName;
  auto fetchConsumer = std::make_shared<NiceMock<MockFetchConsumer>>();
  withSessionContext(fetchSession, [&]() {
    auto task = publisherInterface()->fetch(std::move(joining), fetchConsumer);
    auto res = folly::coro::blockingWait(std::move(task), exec_.get());
    ASSERT_FALSE(res.hasValue());
    EXPECT_EQ(res.error().errorCode, FetchErrorCode::DOES_NOT_EXIST);
  });
  EXPECT_FALSE(upstreamFetched.load()) << "the joining fetch went upstream unresolved";

  removeSession(publisherSession);
  removeSession(fetchSession);
}

// A joining FETCH that names the wrong request for the session's subscription gets an error
// from the relay.
TEST_P(MoQRelayTest, JoiningFetchForWrongRequestFails) {
  auto publisherSession = createMockSession();
  auto subSession = createMockSession();
  doPublish(publisherSession, kTestTrackName);
  auto handle = subscribeToTrack(subSession, kTestTrackName, createMockConsumer(), RequestID(0));
  ASSERT_NE(handle, nullptr);
  std::atomic<bool> upstreamFetched{false};
  failUpstreamFetches(*publisherSession, upstreamFetched);

  Fetch joining(RequestID(2), RequestID(5), /*joiningStart=*/1, FetchType::RELATIVE_JOINING);
  joining.fullTrackName = kTestTrackName;
  auto fetchConsumer = std::make_shared<NiceMock<MockFetchConsumer>>();
  withSessionContext(subSession, [&]() {
    auto task = publisherInterface()->fetch(std::move(joining), fetchConsumer);
    auto res = folly::coro::blockingWait(std::move(task), exec_.get());
    EXPECT_FALSE(res.hasValue());
  });
  EXPECT_FALSE(upstreamFetched.load()) << "the joining fetch went upstream unresolved";

  handle->unsubscribe();
  removeSession(publisherSession);
  removeSession(subSession);
  driveIfMultiThread();
}

// A draft 20 SUBSCRIBE's fill opens on the subscriber's consumer and is served by an
// upstream FETCH that ends at upstream's Largest Object.
TEST_P(MoQRelayTest, SubscribeFillFetchesUpstreamThroughLargest) {
  auto publisherSession = createMockSession();
  auto subSession = createMockSession(kVersionDraft20);
  doPublishNamespace(publisherSession, kTestNamespace);
  answerUpstreamSubscribe(*publisherSession, AbsoluteLocation{5, 0});
  auto upstreamFetch = std::make_shared<Fetch>();
  answerUpstreamFetch(*publisherSession, upstreamFetch);

  auto consumer = createMockConsumer();
  std::atomic<bool> fillEnded{false};
  acceptFill(*consumer, RequestID(1), fillEnded);

  auto sub = makeSubscribeRequest(RequestID(1));
  sub.fill = fillFrom({3, 0});
  auto pending = startSubscribe(sessionExec(), subSession, std::move(sub), consumer);
  ASSERT_TRUE(driveUntil([&] { return pending.ready(); }));
  getOrCreateMockState(subSession)->subscribeHandles.push_back(pending.handle());

  ASSERT_TRUE(driveUntil([&] { return fillEnded.load(); })) << "the fill did not end";
  auto [standalone, joining] = fetchType(*upstreamFetch);
  ASSERT_NE(standalone, nullptr);
  EXPECT_EQ(standalone->start, (AbsoluteLocation{3, 0}));
  EXPECT_EQ(standalone->end, (AbsoluteLocation{5, 1}));

  removeSession(publisherSession);
  removeSession(subSession);
  driveIfMultiThread();
}

// A subscriber that joins an existing subscription gets its fill too.
TEST_P(MoQRelayTest, JoiningSubscriberFillFetchesUpstream) {
  auto publisherSession = createMockSession();
  auto firstSession = createMockSession(kVersionDraft20);
  auto secondSession = createMockSession(kVersionDraft20);
  doPublishNamespace(publisherSession, kTestNamespace);
  answerUpstreamSubscribe(*publisherSession, AbsoluteLocation{5, 0});
  auto upstreamFetch = std::make_shared<Fetch>();
  answerUpstreamFetch(*publisherSession, upstreamFetch);

  ASSERT_NE(
      subscribeToTrack(firstSession, kTestTrackName, createMockConsumer(), RequestID(1)),
      nullptr
  );

  auto consumer = createMockConsumer();
  std::atomic<bool> fillEnded{false};
  acceptFill(*consumer, RequestID(2), fillEnded);
  auto sub = makeSubscribeRequest(RequestID(2));
  sub.fill = fillFrom({4, 0});
  auto pending = startSubscribe(sessionExec(), secondSession, std::move(sub), consumer);
  ASSERT_TRUE(driveUntil([&] { return pending.ready(); }));
  getOrCreateMockState(secondSession)->subscribeHandles.push_back(pending.handle());

  ASSERT_TRUE(driveUntil([&] { return fillEnded.load(); })) << "the fill did not end";
  auto [standalone, joining] = fetchType(*upstreamFetch);
  ASSERT_NE(standalone, nullptr);
  EXPECT_EQ(standalone->start, (AbsoluteLocation{4, 0}));

  removeSession(publisherSession);
  removeSession(firstSession);
  removeSession(secondSession);
  driveIfMultiThread();
}

// A fill requested by REQUEST_UPDATE opens under the update's request ID.
TEST_P(MoQRelayTest, RequestUpdateFillFetchesUpstream) {
  auto publisherSession = createMockSession();
  auto subSession = createMockSession(kVersionDraft20);
  doPublishNamespace(publisherSession, kTestNamespace);
  answerUpstreamSubscribe(*publisherSession, AbsoluteLocation{5, 0});
  auto upstreamFetch = std::make_shared<Fetch>();
  answerUpstreamFetch(*publisherSession, upstreamFetch);

  auto consumer = createMockConsumer();
  auto handle = subscribeToTrack(subSession, kTestTrackName, consumer, RequestID(1));
  ASSERT_NE(handle, nullptr);

  std::atomic<bool> fillEnded{false};
  acceptFill(*consumer, RequestID(3), fillEnded);
  RequestUpdate update;
  update.requestID = RequestID(3);
  update.existingRequestID = RequestID(1);
  update.fill = fillFrom({2, 0});
  withSessionContext(subSession, [&]() {
    auto res = folly::coro::blockingWait(handle->requestUpdate(std::move(update)), exec_.get());
    EXPECT_TRUE(res.hasValue());
  });

  ASSERT_TRUE(driveUntil([&] { return fillEnded.load(); })) << "the fill did not end";
  auto [standalone, joining] = fetchType(*upstreamFetch);
  ASSERT_NE(standalone, nullptr);
  EXPECT_EQ(standalone->start, (AbsoluteLocation{2, 0}));
  EXPECT_EQ(standalone->end, (AbsoluteLocation{5, 1}));

  removeSession(publisherSession);
  removeSession(subSession);
  driveIfMultiThread();
}

namespace {
// Holds each forward=1 REQUEST_UPDATE until the test answers it, and answers the rest at once.
class HeldResumeHandle : public Publisher::SubscriptionHandle {
public:
  HeldResumeHandle() : SubscriptionHandle(makeOk()) {}

  void unsubscribe() override {}

  folly::coro::Task<folly::Expected<RequestOk, RequestError>> requestUpdate(RequestUpdate update
  ) override {
    if (!update.forward.value_or(false)) {
      co_return emptyOk();
    }
    auto [promise, future] = folly::makePromiseContract<folly::Expected<RequestOk, RequestError>>();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      held_.push_back(std::move(promise));
    }
    co_return co_await std::move(future);
  }

  size_t held() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return held_.size();
  }

  // Answers the oldest held resume with `largest` as its Largest Object.
  void replyLargest(AbsoluteLocation largest) {
    auto ok = emptyOk();
    ok.requestSpecificParams.emplace_back(
        folly::to_underlying(TrackRequestParamKey::LARGEST_OBJECT),
        largest
    );
    reply(std::move(ok));
  }

  void replyError() {
    reply(folly::makeUnexpected(
        RequestError(RequestID(0), RequestErrorCode::INTERNAL_ERROR, "resume failed")
    ));
  }

  void failAll() {
    while (held() > 0) {
      replyError();
    }
  }

private:
  static SubscribeOk makeOk() {
    SubscribeOk ok;
    ok.requestID = RequestID(0);
    ok.trackAlias = TrackAlias(0);
    ok.expires = std::chrono::milliseconds(0);
    ok.groupOrder = GroupOrder::Default;
    return ok;
  }

  static RequestOk emptyOk() {
    return RequestOk{.requestID = RequestID(0), .requestSpecificParams = {}, .trackProperties = {}};
  }

  void reply(folly::Expected<RequestOk, RequestError> result) {
    folly::Promise<folly::Expected<RequestOk, RequestError>> promise;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (held_.empty()) {
        ADD_FAILURE() << "no resume is held";
        return;
      }
      promise = std::move(held_.front());
      held_.pop_front();
    }
    promise.setValue(std::move(result));
  }

  mutable std::mutex mutex_;
  std::deque<folly::Promise<folly::Expected<RequestOk, RequestError>>> held_;
};

std::optional<AbsoluteLocation> okLargest(const RequestOk& ok) {
  for (const auto& param : ok.requestSpecificParams) {
    if (param.key == folly::to_underlying(TrackRequestParamKey::LARGEST_OBJECT)) {
      return param.largestObject;
    }
  }
  return std::nullopt;
}
} // namespace

// A PUBLISH whose handle holds each resume, so a test sees what waits on the publisher's
// REQUEST_UPDATE_OK.
class MoqxRelayResumeTest : public MoQRelayTest {
protected:
  void SetUp() override {
    MoQRelayTest::SetUp();
    resetRelay(config::CacheConfig{.maxCachedTracks = 10});
    relay_->setAllowedNamespacePrefix(kAllowedPrefix);
    publisherSession_ = createMockSession();
    publishConsumer_ = doPublishWithHandle(publisherSession_, kTestTrackName, upstream_);
  }

  void TearDown() override {
    upstream_->failAll();
    // Released before the relay thread, which the filter posts its delete to.
    publishConsumer_.reset();
    MoQRelayTest::TearDown();
  }

  // Subscribes, and answers the resume that the SUBSCRIBE starts with `largest`.
  std::shared_ptr<SubscriptionHandle> subscribe(
      const std::shared_ptr<MoQSession>& session,
      std::shared_ptr<TrackConsumer> consumer,
      AbsoluteLocation largest
  ) {
    auto pending = startSubscribe(
        sessionExec(),
        session,
        makeSubscribeRequest(RequestID(1)),
        std::move(consumer)
    );
    if (!driveUntil([&] { return upstream_->held() == 1; })) {
      ADD_FAILURE() << "the SUBSCRIBE did not resume the publisher";
      return nullptr;
    }
    upstream_->replyLargest(largest);
    if (!driveUntil([&] { return pending.ready(); })) {
      ADD_FAILURE() << "the SUBSCRIBE did not finish";
      return nullptr;
    }
    return pending.handle();
  }

  // Starts a REQUEST_UPDATE on `exec` without waiting for its OK.
  folly::SemiFuture<folly::Expected<RequestOk, RequestError>> startUpdate(
      const std::shared_ptr<MoQSession>& session,
      const std::shared_ptr<SubscriptionHandle>& handle,
      RequestUpdate update,
      folly::Executor* exec = nullptr
  ) {
    return withSessionContext(session, [&]() {
      return co_withExecutor(
                 folly::getKeepAliveToken(exec ? exec : sessionExec()),
                 handle->requestUpdate(std::move(update))
      )
          .start();
    });
  }

  static RequestUpdate forwardUpdate(RequestID requestID, bool forward) {
    RequestUpdate update;
    update.requestID = requestID;
    update.existingRequestID = RequestID(1);
    update.forward = forward;
    return update;
  }

  // Pauses the subscription, which pauses the publisher.
  void pause(
      const std::shared_ptr<MoQSession>& session,
      const std::shared_ptr<SubscriptionHandle>& handle,
      folly::Executor* exec = nullptr
  ) {
    auto ok = startUpdate(session, handle, forwardUpdate(RequestID(2), false), exec);
    ASSERT_TRUE(driveUntil([&] { return ok.isReady(); })) << "the pause did not finish";
    ASSERT_TRUE(std::move(ok).get().hasValue());
  }

  std::shared_ptr<HeldResumeHandle> upstream_ = std::make_shared<HeldResumeHandle>();
  std::shared_ptr<MockMoQSession> publisherSession_;
  std::shared_ptr<TrackConsumer> publishConsumer_;
};

// A resume's REQUEST_UPDATE_OK waits for the publisher's, and reports its Largest Object.
TEST_P(MoqxRelayResumeTest, ResumeOkWaitsForPublisherReply) {
  auto subSession = createMockSession();
  auto handle = subscribe(subSession, createMockConsumer(), {3, 0});
  ASSERT_NE(handle, nullptr);
  pause(subSession, handle);

  auto ok = startUpdate(subSession, handle, forwardUpdate(RequestID(3), true));
  ASSERT_TRUE(driveUntil([&] { return upstream_->held() == 1; }));
  EXPECT_FALSE(ok.isReady()) << "REQUEST_UPDATE_OK did not wait for the publisher";

  upstream_->replyLargest({5, 2});
  ASSERT_TRUE(driveUntil([&] { return ok.isReady(); }));
  auto res = std::move(ok).get();
  ASSERT_TRUE(res.hasValue());
  EXPECT_EQ(okLargest(*res), (AbsoluteLocation{5, 2}));

  handle->unsubscribe();
  removeSession(subSession);
  removeSession(publisherSession_);
  driveIfMultiThread();
}

// An object that arrives before the publisher's REQUEST_UPDATE_OK is past the Largest
// Object in it, so the downstream OK reports the object.
TEST_P(MoqxRelayResumeTest, ObjectBeforePublisherReplyWins) {
  auto subSession = createMockSession();
  auto consumer = createMockConsumer();
  std::atomic<bool> gotObject{false};
  ON_CALL(*consumer, beginSubgroup(_, _, _, _))
      .WillByDefault([this, &gotObject](auto, auto, auto, auto) {
        auto sg = createMockSubgroupConsumer();
        ON_CALL(*sg, object(_, _, _, _)).WillByDefault([&gotObject](auto, auto, auto, auto) {
          gotObject.store(true);
          return folly::makeExpected<MoQPublishError>(folly::unit);
        });
        return folly::makeExpected<MoQPublishError>(std::shared_ptr<SubgroupConsumer>(sg));
      });
  auto handle = subscribe(subSession, consumer, {3, 0});
  ASSERT_NE(handle, nullptr);
  pause(subSession, handle);

  auto ok = startUpdate(subSession, handle, forwardUpdate(RequestID(3), true));
  ASSERT_TRUE(driveUntil([&] { return upstream_->held() == 1; }));
  auto sg = publishConsumer_->beginSubgroup(5, 0, 0);
  ASSERT_TRUE(sg.hasValue()) << sg.error().describe();
  EXPECT_TRUE(sg.value()->object(3, folly::IOBuf::copyBuffer("abc")).hasValue());
  ASSERT_TRUE(driveUntil([&] { return gotObject.load(); }));

  upstream_->replyLargest({5, 2});
  ASSERT_TRUE(driveUntil([&] { return ok.isReady(); }));
  auto res = std::move(ok).get();
  ASSERT_TRUE(res.hasValue());
  EXPECT_EQ(okLargest(*res), (AbsoluteLocation{5, 3}));

  EXPECT_TRUE(sg.value()->endOfSubgroup().hasValue());
  handle->unsubscribe();
  removeSession(subSession);
  removeSession(publisherSession_);
  driveIfMultiThread();
}

// A failed resume upstream releases the downstream OK with the Largest Object from before
// the pause.
TEST_P(MoqxRelayResumeTest, PublisherErrorReleasesWithOldLargest) {
  auto subSession = createMockSession();
  auto handle = subscribe(subSession, createMockConsumer(), {3, 0});
  ASSERT_NE(handle, nullptr);
  pause(subSession, handle);

  auto ok = startUpdate(subSession, handle, forwardUpdate(RequestID(3), true));
  ASSERT_TRUE(driveUntil([&] { return upstream_->held() == 1; }));
  upstream_->replyError();
  ASSERT_TRUE(driveUntil([&] { return ok.isReady(); }));
  auto res = std::move(ok).get();
  ASSERT_TRUE(res.hasValue());
  EXPECT_EQ(okLargest(*res), (AbsoluteLocation{3, 0}));

  handle->unsubscribe();
  removeSession(subSession);
  removeSession(publisherSession_);
  driveIfMultiThread();
}

// A fill in the resume ends at the publisher's Largest Object, not the one from before
// the pause.
TEST_P(MoqxRelayResumeTest, FillInResumeEndsAtPublisherLargest) {
  auto subSession = createMockSession(kVersionDraft20);
  auto consumer = createMockConsumer();
  auto handle = subscribe(subSession, consumer, {3, 0});
  ASSERT_NE(handle, nullptr);
  pause(subSession, handle);

  auto upstreamFetch = std::make_shared<Fetch>();
  answerUpstreamFetch(*publisherSession_, upstreamFetch);
  std::atomic<bool> fillEnded{false};
  acceptFill(*consumer, RequestID(3), fillEnded);
  auto update = forwardUpdate(RequestID(3), true);
  update.fill = fillFrom({1, 0});
  auto ok = startUpdate(subSession, handle, std::move(update));
  ASSERT_TRUE(driveUntil([&] { return upstream_->held() == 1; }));
  upstream_->replyLargest({5, 2});

  ASSERT_TRUE(driveUntil([&] { return ok.isReady() && fillEnded.load(); }))
      << "the resume or its fill did not finish";
  auto [standalone, joining] = fetchType(*upstreamFetch);
  ASSERT_NE(standalone, nullptr);
  EXPECT_EQ(standalone->start, (AbsoluteLocation{1, 0}));
  EXPECT_EQ(standalone->end, (AbsoluteLocation{5, 3}));

  handle->unsubscribe();
  removeSession(subSession);
  removeSession(publisherSession_);
  driveIfMultiThread();
}

// With the subscriber on its own thread, LocalForwarder resumes through the channel to the
// publisher's forwarder, and the OK still waits for the publisher.
TEST_P(MoqxRelayResumeTest, CrossThreadResumeWaitsForPublisherReply) {
  auto& subAux = makeAuxExec("resume-sub");
  auto subSession = createMockSessionOn(subAux.exec);
  auto pending = launchSubscribeOn(
      subAux.evb,
      subSession,
      makeSubscribeRequest(RequestID(1)),
      createMockConsumer()
  );
  ASSERT_TRUE(driveUntil([&] { return upstream_->held() == 1; }));
  upstream_->replyLargest({3, 0});
  ASSERT_TRUE(driveUntil([&] { return pending.ready(); }));
  auto handle = pending.handle();
  ASSERT_NE(handle, nullptr);
  pause(subSession, handle, subAux.evb);

  auto ok = startUpdate(subSession, handle, forwardUpdate(RequestID(3), true), subAux.evb);
  ASSERT_TRUE(driveUntil([&] { return upstream_->held() == 1; }));
  EXPECT_FALSE(ok.isReady()) << "REQUEST_UPDATE_OK did not wait for the publisher";

  upstream_->replyLargest({5, 2});
  ASSERT_TRUE(driveUntil([&] { return ok.isReady(); }));
  auto res = std::move(ok).get();
  ASSERT_TRUE(res.hasValue());
  EXPECT_EQ(okLargest(*res), (AbsoluteLocation{5, 2}));

  getOrCreateMockState(subSession)->subscribeHandles.push_back(handle);
  removeSession(publisherSession_);
  removeSession(subSession);
  driveIfMultiThread();
  subAux.evb->runInEventBaseThreadAndWait([] {});
}

INSTANTIATE_TEST_SUITE_P(
    AllModes,
    MoqxRelayResumeTest,
    ::testing::Values(RelayMode::SingleThread, RelayMode::MultiThread, RelayMode::LocalForwarderMT),
    [](const ::testing::TestParamInfo<RelayMode>& info) -> std::string {
      switch (info.param) {
      case RelayMode::SingleThread:
        return "SingleThread";
      case RelayMode::MultiThread:
        return "MultiThread";
      case RelayMode::LocalForwarderMT:
        return "LocalForwarderMT";
      }
      return "Unknown";
    }
);

} // namespace openmoq::moqx::test
