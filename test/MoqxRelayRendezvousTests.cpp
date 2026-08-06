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
// MultiThread) and MoqxRelay::joinOrPrepareUpstreamSubscription
// (LocalForwarderMT) both implement the same rendezvous parking/wake/timeout
// flow.

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
