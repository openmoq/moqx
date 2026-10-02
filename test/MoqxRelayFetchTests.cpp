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
    sub.locType = LocationType::LargestObject;
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

} // namespace openmoq::moqx::test
