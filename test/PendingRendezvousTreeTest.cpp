/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PendingRendezvousTree.h"

#include <folly/coro/BlockingWait.h>
#include <folly/coro/Task.h>
#include <folly/executors/InlineExecutor.h>
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

bool waitFor(const std::shared_ptr<RendezvousWaiter>& waiter, std::chrono::milliseconds timeout) {
  auto res = folly::coro::blockingWait(folly::coro::co_awaitTry(waiter->baton.wait(timeout)));
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
  static constexpr SessionId kSession{1};

  static std::shared_ptr<RendezvousWaiter> makeWaiter(SessionId session = kSession) {
    return std::make_shared<RendezvousWaiter>(session);
  }
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

TEST_F(PendingRendezvousTreeTest, WakeUnderNamespaceLeavesAncestorNamespaceParked) {
  FullTrackName ftnParent{TrackNamespace{{"a"}}, "track"};
  FullTrackName ftnChild{TrackNamespace{{"a", "b"}}, "track"};
  auto waiterParent = makeWaiter();
  auto waiterChild = makeWaiter();
  tree_.addWaiter(ftnParent, waiterParent);
  tree_.addWaiter(ftnChild, waiterChild);

  tree_.wakeUnderNamespace(TrackNamespace{{"a", "b"}});

  EXPECT_TRUE(waitFor(waiterChild, kSignaledTimeout));
  EXPECT_FALSE(waitFor(waiterParent, kNotSignaledTimeout))
      << "a publisher at a/b cannot serve a waiter parked at a; only descendants of "
         "a/b may wake";
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

  tree_.eraseWaiter(ftn, waiter1.get());
  tree_.wakeForTrack(ftn);

  EXPECT_FALSE(waitFor(waiter1, kNotSignaledTimeout)) << "erased waiter must not be woken";
  EXPECT_TRUE(waitFor(waiter2, kSignaledTimeout)) << "remaining waiter must still be woken";
}

TEST_F(PendingRendezvousTreeTest, EraseWaiterOnUnknownTrackIsNoOp) {
  FullTrackName ftn{TrackNamespace{{"never", "added"}}, "track"};
  auto waiter = makeWaiter();

  tree_.eraseWaiter(ftn, waiter.get()); // must not crash despite never being added
}

// isWaiting matches on (exact track, session): not a sibling track, not the same
// track name under an ancestor namespace, not another session, and not an erased
// waiter. Woken waiters stay in the tree, so isWaiting stays true until erased.
TEST_F(PendingRendezvousTreeTest, IsWaitingMatchesExactTrackAndSession) {
  FullTrackName ftn{TrackNamespace{{"test", "ns"}}, "track"};
  FullTrackName sibling{TrackNamespace{{"test", "ns"}}, "other"};
  FullTrackName ancestor{TrackNamespace{{"test"}}, "track"};
  FullTrackName unknown{TrackNamespace{{"never", "added"}}, "track"};
  const SessionId parkedA{1};
  const SessionId parkedB{2};
  const SessionId erased{3};
  const SessionId elsewhere{4};

  EXPECT_FALSE(tree_.isWaiting(ftn, parkedA)) << "empty tree";

  auto waiterA = makeWaiter(parkedA);
  auto waiterB = makeWaiter(parkedB);
  auto erasedWaiter = makeWaiter(erased);
  tree_.addWaiter(ftn, waiterA);
  tree_.addWaiter(ftn, waiterB);
  tree_.addWaiter(ftn, erasedWaiter);
  tree_.addWaiter(sibling, makeWaiter(elsewhere));
  tree_.addWaiter(ancestor, makeWaiter(elsewhere));
  tree_.eraseWaiter(ftn, erasedWaiter.get());

  EXPECT_TRUE(tree_.isWaiting(ftn, parkedA));
  EXPECT_TRUE(tree_.isWaiting(ftn, parkedB));
  EXPECT_FALSE(tree_.isWaiting(ftn, erased)) << "erased waiter";
  EXPECT_FALSE(tree_.isWaiting(ftn, elsewhere)) << "session parked only on sibling/ancestor";
  EXPECT_FALSE(tree_.isWaiting(sibling, parkedA)) << "sibling track";
  EXPECT_FALSE(tree_.isWaiting(ancestor, parkedA)) << "same track name, ancestor namespace";
  EXPECT_FALSE(tree_.isWaiting(unknown, parkedA)) << "namespace not in tree";

  tree_.wakeForTrack(ftn);

  EXPECT_TRUE(waiterA->signalled);
  EXPECT_TRUE(waiterB->signalled);
  EXPECT_TRUE(tree_.isWaiting(ftn, parkedA)) << "woken waiters stay in the tree";
  EXPECT_TRUE(tree_.isWaiting(ftn, parkedB)) << "woken waiters stay in the tree";

  tree_.eraseWaiter(ftn, waiterA.get());

  EXPECT_FALSE(tree_.isWaiting(ftn, parkedA));
  EXPECT_TRUE(tree_.isWaiting(ftn, parkedB)) << "erase removes only that waiter";
}

// TimedBaton::signal() throws on a second call, so a repeated wake (e.g. a
// re-PUBLISH before the woken SUBSCRIBE attaches) must skip signalled waiters.
TEST_F(PendingRendezvousTreeTest, WakeForTrackKeepsWaiterAndSignalsOnce) {
  FullTrackName ftn{TrackNamespace{{"test", "ns"}}, "track"};
  auto waiter = makeWaiter();
  tree_.addWaiter(ftn, waiter);

  EXPECT_NO_THROW(tree_.wakeForTrack(ftn));
  EXPECT_NO_THROW(tree_.wakeForTrack(ftn)) << "second wake must not re-signal";
  EXPECT_NO_THROW(tree_.wakeUnderNamespace(ftn.trackNamespace))
      << "namespace wake must not re-signal a track-woken waiter";

  EXPECT_TRUE(waiter->signalled);
  EXPECT_TRUE(tree_.isWaiting(ftn, kSession));
  EXPECT_FALSE(tree_.empty());
  EXPECT_TRUE(waitFor(waiter, kSignaledTimeout));

  tree_.eraseWaiter(ftn, waiter.get());
  EXPECT_TRUE(tree_.empty()) << "erase prunes the now-empty ancestors";
}

// PUBLISH_NAMESPACE-woken waiters also stay in the tree (decision (A)), and only
// descendants of the woken namespace are marked.
TEST_F(PendingRendezvousTreeTest, WakeUnderNamespaceKeepsWaiters) {
  FullTrackName ftnRoot{TrackNamespace{{"a"}}, "track"};
  FullTrackName ftnChild{TrackNamespace{{"a", "b"}}, "track"};
  FullTrackName ftnGrandchild{TrackNamespace{{"a", "b", "c"}}, "other"};
  auto waiterRoot = makeWaiter();
  auto waiterChild = makeWaiter();
  auto waiterGrandchild = makeWaiter();
  tree_.addWaiter(ftnRoot, waiterRoot);
  tree_.addWaiter(ftnChild, waiterChild);
  tree_.addWaiter(ftnGrandchild, waiterGrandchild);

  EXPECT_NO_THROW(tree_.wakeUnderNamespace(TrackNamespace{{"a", "b"}}));
  EXPECT_NO_THROW(tree_.wakeUnderNamespace(TrackNamespace{{"a", "b"}}))
      << "second wake must not re-signal";
  EXPECT_NO_THROW(tree_.wakeForTrack(ftnChild))
      << "track wake must not re-signal a namespace-woken waiter";

  EXPECT_TRUE(waiterChild->signalled);
  EXPECT_TRUE(waiterGrandchild->signalled);
  EXPECT_FALSE(waiterRoot->signalled) << "ancestor namespace is not woken";
  EXPECT_TRUE(tree_.isWaiting(ftnChild, kSession));
  EXPECT_TRUE(tree_.isWaiting(ftnGrandchild, kSession));
  EXPECT_TRUE(tree_.isWaiting(ftnRoot, kSession));
  EXPECT_TRUE(waitFor(waiterChild, kSignaledTimeout));
  EXPECT_TRUE(waitFor(waiterGrandchild, kSignaledTimeout));

  tree_.eraseWaiter(ftnChild, waiterChild.get());
  tree_.eraseWaiter(ftnGrandchild, waiterGrandchild.get());
  tree_.eraseWaiter(ftnRoot, waiterRoot.get());
  EXPECT_TRUE(tree_.empty());
}

// Collect-then-signal regression: the first waiter's continuation resumes inline
// inside signal() and erases every waiter on the track, mutating the waiter list
// and pruning the node. The wake must not touch the tree after signalling, and
// the second waiter (already collected) must still be signalled. Run under ASAN
// to catch a use-after-free.
TEST_F(PendingRendezvousTreeTest, SignalReentersEraseWaiter) {
  FullTrackName ftn{TrackNamespace{{"test", "ns"}}, "track"};
  auto waiterA = makeWaiter(SessionId{1});
  auto waiterB = makeWaiter(SessionId{2});
  tree_.addWaiter(ftn, waiterA);
  tree_.addWaiter(ftn, waiterB);

  bool resumed = false;
  auto onWake = [](PendingRendezvousTree& tree,
                   FullTrackName ftn,
                   std::shared_ptr<RendezvousWaiter> a,
                   std::shared_ptr<RendezvousWaiter> b,
                   bool& resumed) -> folly::coro::Task<void> {
    co_await a->baton.wait();
    resumed = true;
    tree.eraseWaiter(ftn, a.get());
    tree.eraseWaiter(ftn, b.get());
  };
  folly::coro::co_withExecutor(
      &folly::InlineExecutor::instance(),
      onWake(tree_, ftn, waiterA, waiterB, resumed)
  )
      .start();
  ASSERT_FALSE(resumed);

  tree_.wakeForTrack(ftn);

  EXPECT_TRUE(resumed) << "continuation should resume inline inside signal()";
  EXPECT_TRUE(waiterB->signalled);
  EXPECT_TRUE(tree_.empty());
  EXPECT_TRUE(waitFor(waiterB, kSignaledTimeout))
      << "waiter collected before the reentrant erase must still be signalled";
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
