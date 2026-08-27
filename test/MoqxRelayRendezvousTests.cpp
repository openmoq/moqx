/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Draft 18+: SUBSCRIBE with RENDEZVOUS_TIMEOUT relay tests.
//
// Basic sanity coverage for the rendezvous-timeout feature (parking a
// SUBSCRIBE that arrives before any matching PUBLISH/PUBLISH_NAMESPACE, and
// resolving or timing it out later).
//
// Covers all three relay modes: MoqxRelay::subscribeImpl (SingleThread /
// MultiThread) and MoqxRelay::subscribeFromSubscriberExec (LocalForwarderMT)
// both implement the same rendezvous parking/wake/timeout flow.

#include "MoqxRelayTestFixture.h"

namespace openmoq::moqx::test {

class MoqxRelayRendezvousTest : public MoQRelayTest {
protected:
  // Like createMockSession() but reports draft 18 for the negotiated version,
  // required for a downstream SUBSCRIBE to be rendezvous-eligible.
  std::shared_ptr<MockMoQSession> createV18Session() {
    auto session = std::make_shared<NiceMock<MockMoQSession>>(exec_);
    ON_CALL(*session, getNegotiatedVersion())
        .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft18)));
    getOrCreateMockState(session);
    return session;
  }

  static SubscribeRequest
  makeRendezvousSubscribeRequest(const FullTrackName& ftn, uint64_t timeoutMs) {
    SubscribeRequest sub;
    sub.fullTrackName = ftn;
    sub.requestID = RequestID(1);
    sub.locType = LocationType::LargestObject;
    sub.params.setMajorVersion(getDraftMajorVersion(kVersionDraft18));
    EXPECT_TRUE(
        sub.params
            .insertParam(
                Parameter(folly::to_underlying(TrackRequestParamKey::RENDEZVOUS_TIMEOUT), timeoutMs)
            )
            .hasValue()
    );
    return sub;
  }

  // A v18 SUBSCRIBE with no RENDEZVOUS_TIMEOUT: eligible session, ineligible request.
  static SubscribeRequest makePlainV18SubscribeRequest(const FullTrackName& ftn) {
    SubscribeRequest sub;
    sub.fullTrackName = ftn;
    sub.requestID = RequestID(1);
    sub.locType = LocationType::LargestObject;
    sub.params.setMajorVersion(getDraftMajorVersion(kVersionDraft18));
    return sub;
  }

  // Key 0x04 on a pre-v18 SUBSCRIBE, where it means MAX_CACHE_DURATION rather than
  // RENDEZVOUS_TIMEOUT.
  static SubscribeRequest
  makeMaxCacheDurationSubscribeRequest(const FullTrackName& ftn, uint64_t durationMs) {
    SubscribeRequest sub;
    sub.fullTrackName = ftn;
    sub.requestID = RequestID(1);
    sub.locType = LocationType::LargestObject;
    sub.params.setMajorVersion(getDraftMajorVersion(kVersionDraftCurrent));
    EXPECT_TRUE(sub.params
                    .insertParam(Parameter(
                        folly::to_underlying(TrackRequestParamKey::MAX_CACHE_DURATION),
                        durationMs
                    ))
                    .hasValue());
    return sub;
  }

  // Answers the relay's upstream SUBSCRIBE with a canned OK, recording whether the
  // request still carried key 0x04.
  void expectUpstreamSubscribe(
      const std::shared_ptr<MockMoQSession>& publisherSession,
      bool& sawUpstream,
      bool* sawKey04 = nullptr
  ) {
    SubscribeOk upstreamOk;
    upstreamOk.requestID = RequestID(1);
    upstreamOk.trackAlias = TrackAlias(1);
    upstreamOk.expires = std::chrono::milliseconds(0);
    upstreamOk.groupOrder = GroupOrder::OldestFirst;
    EXPECT_CALL(*publisherSession, subscribe(_, _))
        .WillRepeatedly([&sawUpstream,
                         sawKey04,
                         upstreamOk](const SubscribeRequest& req, std::shared_ptr<TrackConsumer>) {
          sawUpstream = true;
          if (sawKey04) {
            *sawKey04 =
                req.params.getFirstParam(TrackRequestParamKey::MAX_CACHE_DURATION) != nullptr;
          }
          auto handle = std::make_shared<NiceMock<MockSubscriptionHandle>>(upstreamOk);
          return folly::coro::makeTask<Publisher::SubscribeResult>(
              folly::Expected<std::shared_ptr<SubscriptionHandle>, SubscribeError>(handle)
          );
        });
  }

  // Starts a SUBSCRIBE on exec_ without waiting for it, so the test can assert it
  // is still parked.
  auto startSubscribe(
      const std::shared_ptr<MockMoQSession>& session,
      SubscribeRequest sub,
      const std::shared_ptr<MockTrackConsumer>& consumer
  ) {
    return withSessionContext(session, [&]() {
      auto task = publisherInterface()->subscribe(std::move(sub), consumer);
      return co_withExecutor(static_cast<folly::DrivableExecutor*>(exec_.get()), std::move(task))
          .start();
    });
  }
};

// Pre-draft-18 sessions aren't rendezvous-eligible: a SUBSCRIBE for a track
// with no publisher fails immediately instead of parking.
TEST_P(MoqxRelayRendezvousTest, PreV18SessionRejectedImmediately) {
  auto subSession = createMockSession(); // defaults to kVersionDraftCurrent (< 18)
  auto consumer = createMockConsumer();
  auto sub = makeRendezvousSubscribeRequest(kTestTrackName, /*timeoutMs=*/1000);

  auto result = withSessionContext(subSession, [&]() {
    auto task = publisherInterface()->subscribe(std::move(sub), consumer);
    return folly::coro::blockingWait(std::move(task), exec_.get());
  });

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error().errorCode, SubscribeErrorCode::DOES_NOT_EXIST);

  removeSession(subSession);
  driveIfMultiThread();
}

// RENDEZVOUS_TIMEOUT=0 is treated the same as not being present: reject
// immediately rather than parking.
TEST_P(MoqxRelayRendezvousTest, ZeroTimeoutRejectedImmediately) {
  auto subSession = createV18Session();
  auto consumer = createMockConsumer();
  auto sub = makeRendezvousSubscribeRequest(kTestTrackName, /*timeoutMs=*/0);

  auto result = withSessionContext(subSession, [&]() {
    auto task = publisherInterface()->subscribe(std::move(sub), consumer);
    return folly::coro::blockingWait(std::move(task), exec_.get());
  });

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error().errorCode, SubscribeErrorCode::DOES_NOT_EXIST);

  removeSession(subSession);
  driveIfMultiThread();
}

// Core happy path: a rendezvous-eligible SUBSCRIBE for a not-yet-published
// track parks, and a subsequent PUBLISH of that exact track wakes it up and
// completes the subscription.
TEST_P(MoqxRelayRendezvousTest, ResolvesWhenTrackPublished) {
  auto subSession = createV18Session();
  auto pubSession = createMockSession();
  auto consumer = createMockConsumer();
  auto sub = makeRendezvousSubscribeRequest(kTestTrackName, /*timeoutMs=*/5000);

  auto future = withSessionContext(subSession, [&]() {
    auto task = publisherInterface()->subscribe(std::move(sub), consumer);
    return co_withExecutor(static_cast<folly::DrivableExecutor*>(exec_.get()), std::move(task))
        .start();
  });

  exec_->driveFor(10);
  ASSERT_FALSE(future.isReady()) << "subscribe should be parked awaiting rendezvous";

  doPublish(pubSession, kTestTrackName);

  exec_->driveFor(20);
  ASSERT_TRUE(future.isReady()) << "PUBLISH of the exact track should wake the parked subscribe";
  auto result = std::move(future).value();
  ASSERT_TRUE(result.hasValue());

  result.value()->unsubscribe();
  removeSession(pubSession);
  removeSession(subSession);
  driveIfMultiThread();
}

// If nothing ever publishes the track, the parked SUBSCRIBE fails with
// TIMEOUT once RENDEZVOUS_TIMEOUT elapses.
TEST_P(MoqxRelayRendezvousTest, TimesOutWithoutPublish) {
  auto subSession = createV18Session();
  auto consumer = createMockConsumer();
  auto sub = makeRendezvousSubscribeRequest(kTestTrackName, /*timeoutMs=*/50);

  auto result = withSessionContext(subSession, [&]() {
    auto task = publisherInterface()->subscribe(std::move(sub), consumer);
    return folly::coro::blockingWait(std::move(task), exec_.get());
  });

  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error().errorCode, SubscribeErrorCode::TIMEOUT);

  removeSession(subSession);
  driveIfMultiThread();
}

// A rendezvous-eligible SUBSCRIBE for a track that's already published must
// resolve immediately instead of parking
TEST_P(MoqxRelayRendezvousTest, AlreadyPublishedResolvesImmediately) {
  auto pubSession = createMockSession();
  doPublish(pubSession, kTestTrackName);
  driveIfMultiThread();

  auto subSession = createV18Session();
  auto consumer = createMockConsumer();
  auto sub = makeRendezvousSubscribeRequest(kTestTrackName, /*timeoutMs=*/5000);

  auto result = withSessionContext(subSession, [&]() {
    auto task = publisherInterface()->subscribe(std::move(sub), consumer);
    return folly::coro::blockingWait(std::move(task), exec_.get());
  });

  ASSERT_TRUE(result.hasValue()
  ) << "subscribe to an already-published track should resolve immediately, not park";

  result.value()->unsubscribe();
  removeSession(subSession);
  removeSession(pubSession);
  driveIfMultiThread();
}

// Regression test for the blocking SUBSCRIBEs for the same
// not-yet-published ftn arrive on the same iothread, one with a long
// RENDEZVOUS_TIMEOUT and one with a short one. Each waiter's deadline must be
// independent — the short one has to resolve on its own timeout instead of being
// serialized behind (or otherwise coupled to) the long one's wait.
TEST_P(MoqxRelayRendezvousTest, IndependentTimeoutsOnSameIothread) {
  auto longSubSession = createV18Session();
  auto shortSubSession = createV18Session();
  auto longConsumer = createMockConsumer();
  auto shortConsumer = createMockConsumer();

  auto longSub = makeRendezvousSubscribeRequest(kTestTrackName, /*timeoutMs=*/4000);
  auto shortSub = makeRendezvousSubscribeRequest(kTestTrackName, /*timeoutMs=*/50);

  auto longFuture = withSessionContext(longSubSession, [&]() {
    auto task = publisherInterface()->subscribe(std::move(longSub), longConsumer);
    return co_withExecutor(static_cast<folly::DrivableExecutor*>(exec_.get()), std::move(task))
        .start();
  });
  auto shortFuture = withSessionContext(shortSubSession, [&]() {
    auto task = publisherInterface()->subscribe(std::move(shortSub), shortConsumer);
    return co_withExecutor(static_cast<folly::DrivableExecutor*>(exec_.get()), std::move(task))
        .start();
  });

  exec_->driveFor(10);
  ASSERT_FALSE(longFuture.isReady()) << "long-timeout subscribe should be parked";
  ASSERT_FALSE(shortFuture.isReady()) << "short-timeout subscribe should be parked";

  auto start = std::chrono::steady_clock::now();
  auto deadline = start + std::chrono::seconds(2);
  while (!shortFuture.isReady() && std::chrono::steady_clock::now() < deadline) {
    exec_->drive();
  }
  ASSERT_TRUE(shortFuture.isReady()) << "short-timeout subscribe never resolved";
  auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_LT(elapsed, std::chrono::milliseconds(2000))
      << "short-timeout subscribe should resolve near its own 50ms deadline, not be "
      << "serialized behind the long-timeout subscribe's 4s wait on the same iothread";

  auto shortResult = std::move(shortFuture).value();
  ASSERT_TRUE(shortResult.hasError());
  EXPECT_EQ(shortResult.error().errorCode, SubscribeErrorCode::TIMEOUT);

  ASSERT_FALSE(longFuture.isReady())
      << "long-timeout subscribe must remain independently parked; it must not be "
      << "resolved or otherwise disturbed as a side effect of the short one timing out";

  // Resolve the long-timeout subscribe too, proving it still wakes correctly
  auto pubSession = createMockSession();
  doPublish(pubSession, kTestTrackName);
  ASSERT_TRUE(driveUntil([&] { return longFuture.isReady(); }))
      << "long-timeout subscribe should wake on PUBLISH";
  auto longResult = std::move(longFuture).value();
  ASSERT_TRUE(longResult.hasValue());
  longResult.value()->unsubscribe();

  removeSession(pubSession);
  removeSession(shortSubSession);
  removeSession(longSubSession);
  driveIfMultiThread();
}

// A PUBLISH_NAMESPACE for the waiter's own namespace resolves it: any track under
// that namespace becomes reachable via the new publisher.
TEST_P(MoqxRelayRendezvousTest, PublishNamespaceWakesParkedSubscribe) {
  auto subSession = createV18Session();
  auto pubSession = createMockSession();
  auto consumer = createMockConsumer();
  bool sawUpstream = false;
  expectUpstreamSubscribe(pubSession, sawUpstream);

  auto future =
      startSubscribe(subSession, makeRendezvousSubscribeRequest(kTestTrackName, 5000), consumer);
  exec_->driveFor(10);
  ASSERT_FALSE(future.isReady()) << "subscribe should be parked awaiting rendezvous";

  doPublishNamespace(pubSession, kTestNamespace);

  ASSERT_TRUE(driveUntil([&] { return future.isReady(); }))
      << "PUBLISH_NAMESPACE for the waiter's namespace should wake it";
  auto result = std::move(future).value();
  ASSERT_TRUE(result.hasValue()) << "woken subscribe should resolve via the new publisher";
  EXPECT_TRUE(sawUpstream);

  result.value()->unsubscribe();
  removeSession(pubSession);
  removeSession(subSession);
  driveIfMultiThread();
}

// findPublisherSession() is a prefix match, so a publisher at an ancestor namespace
// serves deeper tracks. The subtree wake has to reach them.
TEST_P(MoqxRelayRendezvousTest, AncestorPublishNamespaceWakesDeeperWaiter) {
  const TrackNamespace deepNs{{"test", "namespace", "deep"}};
  const FullTrackName deepTrack{deepNs, "track1"};

  auto subSession = createV18Session();
  auto pubSession = createMockSession();
  auto consumer = createMockConsumer();
  bool sawUpstream = false;
  expectUpstreamSubscribe(pubSession, sawUpstream);

  auto future =
      startSubscribe(subSession, makeRendezvousSubscribeRequest(deepTrack, 5000), consumer);
  exec_->driveFor(10);
  ASSERT_FALSE(future.isReady()) << "subscribe should be parked awaiting rendezvous";

  // Announce the ancestor, not the waiter's own namespace.
  doPublishNamespace(pubSession, kTestNamespace);

  ASSERT_TRUE(driveUntil([&] { return future.isReady(); }))
      << "ancestor PUBLISH_NAMESPACE should wake a waiter parked deeper in the trie";
  auto result = std::move(future).value();
  ASSERT_TRUE(result.hasValue());
  EXPECT_TRUE(sawUpstream);

  result.value()->unsubscribe();
  removeSession(pubSession);
  removeSession(subSession);
  driveIfMultiThread();
}

// PUBLISH registers only that track, so it must not wake waiters for a sibling
// track: addPublish() writes publishes_, not publisherSession_, so the namespace
// is still unresolvable for them.
TEST_P(MoqxRelayRendezvousTest, PublishOfDifferentTrackDoesNotWake) {
  auto subSession = createV18Session();
  auto pubSession = createMockSession();
  auto consumer = createMockConsumer();

  auto future =
      startSubscribe(subSession, makeRendezvousSubscribeRequest(kTestTrackName, 4000), consumer);
  exec_->driveFor(10);
  ASSERT_FALSE(future.isReady());

  doPublish(pubSession, FullTrackName{kTestNamespace, "track2"});
  exec_->driveFor(20);
  EXPECT_FALSE(future.isReady()
  ) << "a PUBLISH of a sibling track must not resolve a waiter for track1";

  // Now publish the real track so the test doesn't wait out the 4s timeout.
  doPublish(pubSession, kTestTrackName);
  ASSERT_TRUE(driveUntil([&] { return future.isReady(); }));
  auto result = std::move(future).value();
  ASSERT_TRUE(result.hasValue());

  result.value()->unsubscribe();
  removeSession(pubSession);
  removeSession(subSession);
  driveIfMultiThread();
}

// The case the parking was moved ahead of localReg->join() for: a SUBSCRIBE with no
// RENDEZVOUS_TIMEOUT must fail immediately rather than inherit a parked waiter's
// deadline, and must not disturb that waiter.
TEST_P(MoqxRelayRendezvousTest, NoTimeoutSubscriberFailsWhileAnotherIsParked) {
  auto parkedSession = createV18Session();
  auto plainSession = createV18Session();
  auto parkedConsumer = createMockConsumer();
  auto plainConsumer = createMockConsumer();

  auto parkedFuture = startSubscribe(
      parkedSession,
      makeRendezvousSubscribeRequest(kTestTrackName, 4000),
      parkedConsumer
  );
  exec_->driveFor(10);
  ASSERT_FALSE(parkedFuture.isReady()) << "first subscribe should be parked";

  auto start = std::chrono::steady_clock::now();
  auto plainResult = withSessionContext(plainSession, [&]() {
    auto task = publisherInterface()->subscribe(
        makePlainV18SubscribeRequest(kTestTrackName),
        plainConsumer
    );
    return folly::coro::blockingWait(std::move(task), exec_.get());
  });
  auto elapsed = std::chrono::steady_clock::now() - start;

  ASSERT_TRUE(plainResult.hasError());
  EXPECT_EQ(plainResult.error().errorCode, SubscribeErrorCode::DOES_NOT_EXIST);
  EXPECT_LT(elapsed, std::chrono::milliseconds(1000))
      << "a no-timeout SUBSCRIBE must not be serialized behind the parked one's 4s wait";
  EXPECT_FALSE(parkedFuture.isReady())
      << "the parked waiter must survive the other subscriber's failure";

  auto pubSession = createMockSession();
  doPublish(pubSession, kTestTrackName);
  ASSERT_TRUE(driveUntil([&] { return parkedFuture.isReady(); }))
      << "parked waiter should still wake on PUBLISH after the failed neighbour";
  auto parked = std::move(parkedFuture).value();
  ASSERT_TRUE(parked.hasValue());

  parked.value()->unsubscribe();
  removeSession(pubSession);
  removeSession(plainSession);
  removeSession(parkedSession);
  driveIfMultiThread();
}

// makeUpstreamSubReq() erases key 0x04 unconditionally. Below draft 18 that key is
// MAX_CACHE_DURATION, a legal SUBSCRIBE parameter, so a pre-v18 client's value is
// dropped on the way upstream. moxygen gates the same erase on either side being v18+.
TEST_P(MoqxRelayRendezvousTest, UpstreamSubReqDropsPreV18MaxCacheDuration) {
  auto pubSession = createMockSession(); // draft 14
  auto subSession = createMockSession(); // draft 14
  doPublishNamespace(pubSession, kTestNamespace);

  bool sawUpstream = false;
  bool sawKey04 = false;
  expectUpstreamSubscribe(pubSession, sawUpstream, &sawKey04);

  auto consumer = createMockConsumer();
  auto result = withSessionContext(subSession, [&]() {
    auto task = publisherInterface()->subscribe(
        makeMaxCacheDurationSubscribeRequest(kTestTrackName, /*durationMs=*/30'000),
        consumer
    );
    return folly::coro::blockingWait(std::move(task), exec_.get());
  });

  ASSERT_TRUE(result.hasValue());
  ASSERT_TRUE(sawUpstream) << "relay should have issued an upstream subscribe";
  EXPECT_TRUE(sawKey04) << "MAX_CACHE_DURATION must survive a pre-v18 hop; key 0x04 only means "
                           "RENDEZVOUS_TIMEOUT at draft 18+";

  result.value()->unsubscribe();
  removeSession(subSession);
  removeSession(pubSession);
  driveIfMultiThread();
}

// A publisher announcing the empty namespace claims every track, so waking the whole
// trie is right and each waiter must then route to it. It does not: findNode() refuses
// to prefix-match at the root (`nodePtr.get() != &root_`), so findPublisherSession()
// answers null and the woken waiter fails DOES_NOT_EXIST. moxygen's
// findPublishNamespaceSession() seeds deepestPublisher from the root instead.
TEST_P(MoqxRelayRendezvousTest, EmptyNamespacePublishNamespaceResolvesWaiters) {
  relay_->setAllowedNamespacePrefix(TrackNamespace{});

  auto subSession = createV18Session();
  auto pubSession = createMockSession();
  auto consumer = createMockConsumer();
  bool sawUpstream = false;
  expectUpstreamSubscribe(pubSession, sawUpstream);

  auto future =
      startSubscribe(subSession, makeRendezvousSubscribeRequest(kTestTrackName, 4000), consumer);
  exec_->driveFor(10);
  ASSERT_FALSE(future.isReady());

  doPublishNamespace(pubSession, TrackNamespace{});

  ASSERT_TRUE(driveUntil([&] { return future.isReady(); }))
      << "a root publisher claims every namespace, so the waiter should wake";
  auto result = std::move(future).value();
  EXPECT_TRUE(result.hasValue()
  ) << "a root publisher can serve this track, so the woken waiter must route to it";

  if (result.hasValue()) {
    result.value()->unsubscribe();
  }
  removeSession(pubSession);
  removeSession(subSession);
  driveIfMultiThread();
}

// Port of moxygen a682b57a ("Route SUBSCRIBE past publisher-less namespace nodes"):
// PUBLISH and SUBSCRIBE_NAMESPACE also create nodes, and those carry no publisher, so
// a SUBSCRIBE descending through one must still reach the ancestor that does publish.
// Two variants, matching the two upstream tests.
TEST_P(MoqxRelayRendezvousTest, SubscribeRoutesPastPublishCreatedNode) {
  auto pubSession = createMockSession();
  auto otherPubSession = createMockSession();
  auto subSession = createMockSession();
  bool sawUpstream = false;
  expectUpstreamSubscribe(pubSession, sawUpstream);

  doPublishNamespace(pubSession, kAllowedPrefix);
  // A PUBLISH under "test/namespace" materialises that node with no publisher on it.
  doPublish(otherPubSession, FullTrackName{kTestNamespace, "other"});

  auto consumer = createMockConsumer();
  auto result = withSessionContext(subSession, [&]() {
    auto task =
        publisherInterface()->subscribe(makePlainV18SubscribeRequest(kTestTrackName), consumer);
    return folly::coro::blockingWait(std::move(task), exec_.get());
  });

  EXPECT_TRUE(result.hasValue()
  ) << "a PUBLISH-created node must not hide the publisher at the ancestor";

  if (result.hasValue()) {
    result.value()->unsubscribe();
  }
  removeSession(subSession);
  removeSession(otherPubSession);
  removeSession(pubSession);
  driveIfMultiThread();
}

TEST_P(MoqxRelayRendezvousTest, SubscribeRoutesPastSubscribeNamespaceCreatedNode) {
  auto pubSession = createMockSession();
  auto nsSubSession = createMockSession();
  auto subSession = createMockSession();
  bool sawUpstream = false;
  expectUpstreamSubscribe(pubSession, sawUpstream);

  // Publisher owns the "test" prefix; the track sits a level below it.
  doPublishNamespace(pubSession, kAllowedPrefix);
  // A namespace subscriber materialises "test/namespace" carrying no publisher.
  doSubscribeNamespace(nsSubSession, kTestNamespace);

  auto consumer = createMockConsumer();
  auto result = withSessionContext(subSession, [&]() {
    auto task =
        publisherInterface()->subscribe(makePlainV18SubscribeRequest(kTestTrackName), consumer);
    return folly::coro::blockingWait(std::move(task), exec_.get());
  });

  EXPECT_TRUE(result.hasValue()
  ) << "an empty intermediate node must not hide the publisher at the ancestor";

  if (result.hasValue()) {
    result.value()->unsubscribe();
  }
  removeSession(subSession);
  removeSession(nsSubSession);
  removeSession(pubSession);
  driveIfMultiThread();
}

INSTANTIATE_TEST_SUITE_P(
    AllModes,
    MoqxRelayRendezvousTest,
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
