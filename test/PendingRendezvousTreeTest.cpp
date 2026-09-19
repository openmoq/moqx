/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PendingRendezvousTree.h"

#include <folly/coro/BlockingWait.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <moxygen/MoQVersions.h>
#include <moxygen/test/MockMoQSession.h>

using namespace testing;
using namespace moxygen;
using namespace std::chrono_literals;

namespace openmoq::moqx::test {

namespace {
// Generous enough that a waiter already signaled resolves immediately rather
// than actually waiting; only the negative (not-signaled) checks below spend
// real wall-clock time, capped by kNotSignaledTimeout.
constexpr auto kSignaledTimeout = 5s;
constexpr auto kNotSignaledTimeout = 20ms;

bool waitFor(const std::shared_ptr<TimedBaton>& waiter, std::chrono::milliseconds timeout) {
  auto res = folly::coro::blockingWait(folly::coro::co_awaitTry(waiter->wait(timeout)));
  return !res.hasException();
}

std::shared_ptr<moxygen::test::MockMoQSession> makeV18Session() {
  auto session = std::make_shared<NiceMock<moxygen::test::MockMoQSession>>();
  ON_CALL(*session, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft18)));
  return session;
}
} // namespace

class PendingRendezvousTreeTest : public ::testing::Test {
protected:
  PendingRendezvousTree tree_;

  static std::shared_ptr<TimedBaton> makeWaiter() { return std::make_shared<TimedBaton>(); }
};

TEST_F(PendingRendezvousTreeTest, WakeForTrackSignalsWaiter) {
  FullTrackName ftn{TrackNamespace{{"test", "ns"}}, "track"};
  auto waiter = makeWaiter();
  tree_.addWaiter(ftn, waiter);

  tree_.wakeForTrack(ftn);

  EXPECT_TRUE(waitFor(waiter, kSignaledTimeout));
}

TEST_F(PendingRendezvousTreeTest, WakeForTrackLeavesOtherTrackParked) {
  TrackNamespace ns{{"test", "ns"}};
  FullTrackName ftnA{ns, "trackA"};
  FullTrackName ftnB{ns, "trackB"};
  auto waiterA = makeWaiter();
  auto waiterB = makeWaiter();
  tree_.addWaiter(ftnA, waiterA);
  tree_.addWaiter(ftnB, waiterB);

  tree_.wakeForTrack(ftnA);

  EXPECT_TRUE(waitFor(waiterA, kSignaledTimeout));
  EXPECT_FALSE(waitFor(waiterB, kNotSignaledTimeout)) << "wake for trackA must not wake trackB";
}

TEST_F(PendingRendezvousTreeTest, WakeUnderNamespaceSignalsDescendantWaiters) {
  TrackNamespace ns{{"test"}};
  TrackNamespace childNs{{"test", "child"}};
  FullTrackName ftnRoot{ns, "trackRoot"};
  FullTrackName ftnChild{childNs, "trackChild"};
  auto waiterRoot = makeWaiter();
  auto waiterChild = makeWaiter();
  tree_.addWaiter(ftnRoot, waiterRoot);
  tree_.addWaiter(ftnChild, waiterChild);

  tree_.wakeUnderNamespace(ns);

  EXPECT_TRUE(waitFor(waiterRoot, kSignaledTimeout));
  EXPECT_TRUE(waitFor(waiterChild, kSignaledTimeout))
      << "waking an ancestor namespace must wake every descendant track";
}

TEST_F(PendingRendezvousTreeTest, WakeUnderNamespaceLeavesSiblingNamespaceParked) {
  FullTrackName ftnA{TrackNamespace{{"a"}}, "track"};
  FullTrackName ftnB{TrackNamespace{{"b"}}, "track"};
  auto waiterA = makeWaiter();
  auto waiterB = makeWaiter();
  tree_.addWaiter(ftnA, waiterA);
  tree_.addWaiter(ftnB, waiterB);

  tree_.wakeUnderNamespace(TrackNamespace{{"a"}});

  EXPECT_TRUE(waitFor(waiterA, kSignaledTimeout));
  EXPECT_FALSE(waitFor(waiterB, kNotSignaledTimeout))
      << "waking namespace \"a\" must not wake sibling namespace \"b\"";
}

TEST_F(PendingRendezvousTreeTest, EraseWaiterRemovesOnlyThatWaiter) {
  FullTrackName ftn{TrackNamespace{{"test", "ns"}}, "track"};
  auto waiter1 = makeWaiter();
  auto waiter2 = makeWaiter();
  tree_.addWaiter(ftn, waiter1);
  tree_.addWaiter(ftn, waiter2);

  tree_.eraseWaiter(ftn, waiter1);
  tree_.wakeForTrack(ftn);

  EXPECT_FALSE(waitFor(waiter1, kNotSignaledTimeout)) << "erased waiter must not be woken";
  EXPECT_TRUE(waitFor(waiter2, kSignaledTimeout)) << "remaining waiter must still be woken";
}

TEST_F(PendingRendezvousTreeTest, EraseWaiterOnUnknownTrackIsNoOp) {
  FullTrackName ftn{TrackNamespace{{"never", "added"}}, "track"};
  auto waiter = makeWaiter();

  tree_.eraseWaiter(ftn, waiter); // must not crash despite never being added
}

// Regression coverage for the reentrant wake-while-iterating fix (see
// wakeForTrack's declaration comment): two waiters parked on the same track
// must both resolve from a single wake, since the wake path signals only
// after the tree walk/mutation is fully done.
TEST_F(PendingRendezvousTreeTest, MultipleWaitersOnSameTrackAllSignaledByOneWake) {
  FullTrackName ftn{TrackNamespace{{"test", "ns"}}, "track"};
  auto waiterA = makeWaiter();
  auto waiterB = makeWaiter();
  tree_.addWaiter(ftn, waiterA);
  tree_.addWaiter(ftn, waiterB);

  tree_.wakeForTrack(ftn);

  EXPECT_TRUE(waitFor(waiterA, kSignaledTimeout));
  EXPECT_TRUE(waitFor(waiterB, kSignaledTimeout));
}

// Same reentrancy concern as above, but for wakeUnderNamespace/collectSubtree
// waking waiters parked on different tracks under the same namespace node.
TEST_F(PendingRendezvousTreeTest, MultipleWaitersUnderNamespaceAllSignaledByOneWake) {
  TrackNamespace ns{{"test", "ns"}};
  FullTrackName ftnA{ns, "trackA"};
  FullTrackName ftnB{ns, "trackB"};
  auto waiterA = makeWaiter();
  auto waiterB = makeWaiter();
  tree_.addWaiter(ftnA, waiterA);
  tree_.addWaiter(ftnB, waiterB);

  tree_.wakeUnderNamespace(ns);

  EXPECT_TRUE(waitFor(waiterA, kSignaledTimeout));
  EXPECT_TRUE(waitFor(waiterB, kSignaledTimeout));
}

// RENDEZVOUS_TIMEOUT is clamped to kMaxRendezvousTimeout regardless of how
// large the client requests it.
TEST_F(PendingRendezvousTreeTest, TakeRendezvousTimeoutClampedToMax) {
  auto session = makeV18Session();
  SubscribeRequest sub;
  sub.fullTrackName = FullTrackName{TrackNamespace{{"test", "ns"}}, "track"};
  sub.params.setMajorVersion(getDraftMajorVersion(kVersionDraft18));
  ASSERT_TRUE(sub.params
                  .insertParam(Parameter(
                      folly::to_underlying(TrackRequestParamKey::RENDEZVOUS_TIMEOUT),
                      static_cast<uint64_t>(kMaxRendezvousTimeout.count()) + 60'000
                  ))
                  .hasValue());

  auto clamped = takeRendezvousTimeout(sub, session);

  ASSERT_TRUE(clamped.has_value());
  EXPECT_EQ(*clamped, kMaxRendezvousTimeout);
  EXPECT_EQ(sub.params.getFirstParam(TrackRequestParamKey::RENDEZVOUS_TIMEOUT), nullptr)
      << "the per-hop param must be consumed regardless of clamping";
}

// A request under the ceiling passes through unclamped.
TEST_F(PendingRendezvousTreeTest, TakeRendezvousTimeoutUnderMaxPassesThrough) {
  auto session = makeV18Session();
  SubscribeRequest sub;
  sub.fullTrackName = FullTrackName{TrackNamespace{{"test", "ns"}}, "track"};
  sub.params.setMajorVersion(getDraftMajorVersion(kVersionDraft18));
  ASSERT_TRUE(
      sub.params
          .insertParam(
              Parameter(folly::to_underlying(TrackRequestParamKey::RENDEZVOUS_TIMEOUT), 5000)
          )
          .hasValue()
  );

  auto timeout = takeRendezvousTimeout(sub, session);

  ASSERT_TRUE(timeout.has_value());
  EXPECT_EQ(*timeout, std::chrono::milliseconds(5000));
}

} // namespace openmoq::moqx::test
