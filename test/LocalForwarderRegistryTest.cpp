/*
 * Copyright (c) OpenMOQ contributors.
 */

#include "relay/LocalForwarderRegistry.h"

#include <folly/executors/InlineExecutor.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>

using namespace testing;
using namespace moxygen;

namespace {

using openmoq::moqx::InitialTrackState;
using Registry = openmoq::moqx::LocalForwarderRegistry;
using Claim = Registry::Claim;
using Absent = Registry::Absent;
using Pending = Registry::Pending;
using Ready = Registry::Ready;

const TrackNamespace kTestNs{{"test", "namespace"}};
const FullTrackName kFtn{kTestNs, "track1"};
const FullTrackName kOtherFtn{kTestNs, "track2"};

std::shared_ptr<MoQForwarder> makeForwarder() {
  return std::make_shared<MoQForwarder>(kFtn);
}

struct DoneWatcher : public MoQForwarder::Callback {
  void onEmpty(MoQForwarder*) override {}
  void onPublishDone(MoQForwarder*) override { sawPublishDone = true; }
  bool sawPublishDone{false};
};

Registry::JoinResult joinWith(Registry& reg, const std::shared_ptr<MoQForwarder>& fwd) {
  return reg.join(kFtn, [&] { return fwd; });
}

Claim claimWith(Registry& reg, const std::shared_ptr<MoQForwarder>& fwd) {
  auto result = joinWith(reg, fwd);
  EXPECT_TRUE(std::holds_alternative<Claim>(result));
  return std::move(std::get<Claim>(result));
}

// A waiter's view of the entry, taken while it is pending.
folly::SemiFuture<folly::Unit> waiterOn(Registry& reg) {
  auto state = reg.lookup(kFtn);
  EXPECT_TRUE(std::holds_alternative<Pending>(state));
  return std::move(std::get<Pending>(state).ready);
}

// The registry resolves inline, so a ready future needs no executor to inspect.
folly::Try<folly::Unit> settled(folly::SemiFuture<folly::Unit> f) {
  EXPECT_TRUE(f.isReady());
  return std::move(f).getTry();
}

// === States and transitions ===

TEST(LocalForwarderRegistryTest, EmptyRegistryReadsAbsent) {
  Registry reg;
  EXPECT_TRUE(std::holds_alternative<Absent>(reg.lookup(kFtn)));
  EXPECT_EQ(reg.getIfReady(kFtn), nullptr);
  EXPECT_EQ(reg.getForEviction(kFtn), nullptr);
}

TEST(LocalForwarderRegistryTest, JoinOnAbsentClaimsAPendingEntry) {
  Registry reg;
  auto fwd = makeForwarder();
  auto claim = claimWith(reg, fwd);

  EXPECT_TRUE(static_cast<bool>(claim));
  EXPECT_EQ(claim.forwarder(), fwd);
  EXPECT_TRUE(std::holds_alternative<Pending>(reg.lookup(kFtn)));

  claim.markReady(InitialTrackState{});
}

// The invariant the type exists for: a reader cannot obtain a forwarder before it is ready.
TEST(LocalForwarderRegistryTest, PendingForwarderIsInvisibleToReaders) {
  Registry reg;
  auto claim = claimWith(reg, makeForwarder());

  EXPECT_EQ(reg.getIfReady(kFtn), nullptr);
  auto state = reg.lookup(kFtn);
  EXPECT_TRUE(std::holds_alternative<Pending>(state));

  claim.markReady(InitialTrackState{});
}

TEST(LocalForwarderRegistryTest, SecondJoinWhilePendingWaitsRatherThanAttaching) {
  Registry reg;
  auto claim = claimWith(reg, makeForwarder());

  bool secondFactoryRan = false;
  auto joined = reg.join(kFtn, [&] {
    secondFactoryRan = true;
    return makeForwarder();
  });
  EXPECT_FALSE(secondFactoryRan);
  ASSERT_TRUE(std::holds_alternative<Pending>(joined));
  EXPECT_FALSE(std::get<Pending>(joined).ready.isReady());

  claim.markReady(InitialTrackState{});
  EXPECT_TRUE(settled(std::move(std::get<Pending>(joined).ready)).hasValue());
}

// The reason markReady() takes the state: a waiter released by it can never observe the
// forwarder before its initial state was set.
TEST(LocalForwarderRegistryTest, MarkReadyAppliesInitialStateBeforeReleasingWaiters) {
  Registry reg;
  auto fwd = makeForwarder();
  auto claim = claimWith(reg, fwd);
  auto waiter = waiterOn(reg);
  ASSERT_FALSE(fwd->largest().has_value());

  // Inline executor: the continuation runs inside markReady()'s setValue, so it samples the
  // forwarder at the instant waiters are released rather than afterwards.
  std::optional<AbsoluteLocation> seenByWaiter;
  auto observed =
      std::move(waiter).via(&folly::InlineExecutor::instance()).thenValue([&](folly::Unit) {
        seenByWaiter = fwd->largest();
      });

  claim.markReady(InitialTrackState{AbsoluteLocation{5, 2}, Extensions{}});

  ASSERT_TRUE(observed.isReady());
  EXPECT_EQ(seenByWaiter, (AbsoluteLocation{5, 2}));
}

// The two are separate so a caller can finish setup in between; the assert keeps them ordered.
TEST(LocalForwarderRegistryTest, SetInitialStateThenMarkReadyReleasesWaiters) {
  Registry reg;
  auto fwd = makeForwarder();
  auto claim = claimWith(reg, fwd);
  auto waiter = waiterOn(reg);

  claim.setInitialState(InitialTrackState{AbsoluteLocation{9, 1}, Extensions{}});
  EXPECT_FALSE(waiter.isReady());
  EXPECT_EQ(reg.getIfReady(kFtn), nullptr);

  claim.markReady();

  EXPECT_TRUE(settled(std::move(waiter)).hasValue());
  EXPECT_EQ(fwd->largest(), (AbsoluteLocation{9, 1}));
}

TEST(LocalForwarderRegistryDeathTest, MarkReadyWithoutInitialStateAborts) {
  EXPECT_DEATH(
      {
        Registry reg;
        claimWith(reg, makeForwarder()).markReady();
      },
      "markReady\\(\\) before setInitialState\\(\\)"
  );
}

TEST(LocalForwarderRegistryTest, MarkReadyExposesForwarderAndReleasesWaiters) {
  Registry reg;
  auto fwd = makeForwarder();
  auto claim = claimWith(reg, fwd);
  auto waiter = waiterOn(reg);

  claim.markReady(InitialTrackState{});

  EXPECT_TRUE(settled(std::move(waiter)).hasValue());
  EXPECT_EQ(reg.getIfReady(kFtn), fwd);
  auto state = reg.lookup(kFtn);
  ASSERT_TRUE(std::holds_alternative<Ready>(state));
  EXPECT_EQ(std::get<Ready>(state).forwarder, fwd);
}

// A failed setup vacates the entry: leaving the forwarder behind would make it
// indistinguishable from a ready one to every later reader.
TEST(LocalForwarderRegistryTest, FailMakesEntryAbsentAndFailsWaiters) {
  Registry reg;
  auto claim = claimWith(reg, makeForwarder());
  auto waiter = waiterOn(reg);

  claim.fail(std::runtime_error("upstream subscribe failed"));

  EXPECT_TRUE(settled(std::move(waiter)).hasException());
  EXPECT_TRUE(std::holds_alternative<Absent>(reg.lookup(kFtn)));
  EXPECT_EQ(reg.getIfReady(kFtn), nullptr);
  EXPECT_EQ(reg.getForEviction(kFtn), nullptr);
}

TEST(LocalForwarderRegistryTest, JoinAfterFailedSetupRebuildsTheEntry) {
  Registry reg;
  claimWith(reg, makeForwarder()).fail(std::runtime_error("setup failed"));

  auto second = makeForwarder();
  auto claim = claimWith(reg, second);
  EXPECT_EQ(claim.forwarder(), second);
  claim.markReady(InitialTrackState{});
  EXPECT_EQ(reg.getIfReady(kFtn), second);
}

// An abandoned setup must not strand the waiters parked on it.
TEST(LocalForwarderRegistryTest, DroppedClaimFailsWaiters) {
  Registry reg;
  auto waiter = folly::SemiFuture<folly::Unit>::makeEmpty();
  {
    auto claim = claimWith(reg, makeForwarder());
    waiter = waiterOn(reg);
  }
  EXPECT_TRUE(settled(std::move(waiter)).hasException());
  EXPECT_TRUE(std::holds_alternative<Absent>(reg.lookup(kFtn)));
}

TEST(LocalForwarderRegistryTest, JoinOnReadyEntryAttachesWithoutCallingFactory) {
  Registry reg;
  auto fwd = makeForwarder();
  claimWith(reg, fwd).markReady(InitialTrackState{});

  bool factoryRan = false;
  auto joined = reg.join(kFtn, [&] {
    factoryRan = true;
    return makeForwarder();
  });
  EXPECT_FALSE(factoryRan);
  ASSERT_TRUE(std::holds_alternative<Ready>(joined));
  EXPECT_EQ(std::get<Ready>(joined).forwarder, fwd);
}

// === Identity ===

// The production displacement: a publisher's forwarder replaces a ready subscribe-path
// one. Takes the branch where there are no waiters to fail.
TEST(LocalForwarderRegistryTest, ReplaceOverReadyEntryInstallsSuccessor) {
  Registry reg;
  auto displaced = makeForwarder();
  claimWith(reg, displaced).markReady(InitialTrackState{});

  auto successor = makeForwarder();
  reg.replace(kFtn, successor).markReady(InitialTrackState{AbsoluteLocation{3, 0}, Extensions{}});

  EXPECT_EQ(reg.getIfReady(kFtn), successor);
  EXPECT_EQ(successor->largest(), (AbsoluteLocation{3, 0}));

  // The displaced forwarder drains later; its removal must not evict the successor.
  reg.remove(kFtn, displaced.get());
  EXPECT_EQ(reg.getIfReady(kFtn), successor);
}

TEST(LocalForwarderRegistryTest, ReplaceFailsDisplacedWaiters) {
  Registry reg;
  auto displaced = claimWith(reg, makeForwarder());
  auto waiter = waiterOn(reg);

  auto claim = reg.replace(kFtn, makeForwarder());

  EXPECT_TRUE(settled(std::move(waiter)).hasException());
  claim.markReady(InitialTrackState{});
}

// The bug identity checks exist for: a displaced owner must not release its
// successor's waiters onto a forwarder that no longer owns the entry.
TEST(LocalForwarderRegistryTest, StaleClaimMarkReadyDoesNotReleaseSuccessorsWaiters) {
  Registry reg;
  auto stale = claimWith(reg, makeForwarder());
  auto successorFwd = makeForwarder();
  auto successor = reg.replace(kFtn, successorFwd);
  auto waiter = waiterOn(reg);

  stale.markReady(InitialTrackState{});

  EXPECT_FALSE(waiter.isReady());
  EXPECT_TRUE(std::holds_alternative<Pending>(reg.lookup(kFtn)));

  successor.markReady(InitialTrackState{});
  EXPECT_TRUE(settled(std::move(waiter)).hasValue());
  EXPECT_EQ(reg.getIfReady(kFtn), successorFwd);
}

TEST(LocalForwarderRegistryTest, StaleClaimFailDoesNotEvictSuccessor) {
  Registry reg;
  auto stale = claimWith(reg, makeForwarder());
  auto successorFwd = makeForwarder();
  auto successor = reg.replace(kFtn, successorFwd);

  stale.fail(std::runtime_error("stale setup failed"));

  EXPECT_TRUE(std::holds_alternative<Pending>(reg.lookup(kFtn)));
  successor.markReady(InitialTrackState{});
  EXPECT_EQ(reg.getIfReady(kFtn), successorFwd);
}

TEST(LocalForwarderRegistryTest, RemoveIsIdentityChecked) {
  Registry reg;
  auto fwd = makeForwarder();
  claimWith(reg, fwd).markReady(InitialTrackState{});

  auto other = makeForwarder();
  reg.remove(kFtn, other.get());
  EXPECT_EQ(reg.getIfReady(kFtn), fwd);

  reg.remove(kFtn, fwd.get());
  EXPECT_TRUE(std::holds_alternative<Absent>(reg.lookup(kFtn)));
}

TEST(LocalForwarderRegistryTest, RemoveOfPendingEntryWakesWaiters) {
  Registry reg;
  auto claim = claimWith(reg, makeForwarder());
  auto waiter = waiterOn(reg);

  reg.remove(kFtn, claim.forwarder().get());

  EXPECT_TRUE(settled(std::move(waiter)).hasException());
  EXPECT_TRUE(std::holds_alternative<Absent>(reg.lookup(kFtn)));
  // The claim now owns nothing; resolving it must not resurrect the entry.
  claim.markReady(InitialTrackState{});
  EXPECT_TRUE(std::holds_alternative<Absent>(reg.lookup(kFtn)));
}

// === Displacement parking ===

// The entry holds the only long-lived ref, so parking is what keeps the displaced
// occupant alive on this thread while its owner's decision travels to another one.
TEST(LocalForwarderRegistryTest, ReplaceAndParkHoldsTheDisplacedForwarder) {
  Registry reg;
  auto first = makeForwarder();
  claimWith(reg, first).markReady(InitialTrackState{});
  std::weak_ptr<MoQForwarder> parked = first;
  first.reset();

  auto successor = reg.replaceAndPark(kFtn, makeForwarder());
  EXPECT_FALSE(parked.expired());
  EXPECT_TRUE(static_cast<bool>(successor.displaced)) << "the call hands back a live debt";

  auto displaced = reg.takeDisplaced(kFtn, std::move(successor.displaced));
  ASSERT_NE(displaced, nullptr);
  EXPECT_EQ(displaced, parked.lock());
  // Redeeming consumed the ticket, so taking twice is not expressible.
  EXPECT_FALSE(static_cast<bool>(successor.displaced));

  successor.claim.markReady(InitialTrackState{});
}

// Parking is opt-in: a caller already on the owning thread can drop the occupant itself,
// and owes nothing afterwards.
TEST(LocalForwarderRegistryTest, PlainReplaceParksNothing) {
  Registry reg;
  auto first = makeForwarder();
  claimWith(reg, first).markReady(InitialTrackState{});
  std::weak_ptr<MoQForwarder> dropped = first;
  first.reset();

  reg.replace(kFtn, makeForwarder()).markReady(InitialTrackState{});

  EXPECT_TRUE(dropped.expired());
  EXPECT_EQ(reg.takeDisplaced(kFtn, Registry::ParkTicket{}), nullptr);
}

// Parking an absent entry displaces nothing, so the ticket comes back empty. Callers
// branch on that to decide whether they owe anything, and an empty one costs nothing
// to drop.
TEST(LocalForwarderRegistryTest, ParkingAnAbsentEntryOwesNothing) {
  Registry reg;
  auto result = reg.replaceAndPark(kFtn, makeForwarder());

  EXPECT_FALSE(static_cast<bool>(result.displaced));
  result.claim.markReady(InitialTrackState{});
  // result.displaced goes out of scope here unredeemed, which must not abort.
}

// Displacing a pending entry is the intricate case: its waiters are failed, its
// forwarder is parked, and the previous Claim is still out there holding a strong ref
// and owing a resolution. That stale Claim must not disturb the park.
TEST(LocalForwarderRegistryTest, ReplaceAndParkDisplacesAPendingEntry) {
  Registry reg;
  auto first = makeForwarder();
  auto stale = claimWith(reg, first); // deliberately left pending
  auto waiter = waiterOn(reg);
  std::weak_ptr<MoQForwarder> parked = first;
  first.reset();

  auto successor = reg.replaceAndPark(kFtn, makeForwarder());
  EXPECT_TRUE(settled(std::move(waiter)).hasException())
      << "a displaced entry's waiters are failed, not inherited";

  {
    auto dropped = std::move(stale);
  } // resolves against an entry it no longer owns
  EXPECT_FALSE(parked.expired()) << "the park still anchors it";

  auto displaced = reg.takeDisplaced(kFtn, std::move(successor.displaced));
  ASSERT_NE(displaced, nullptr);
  EXPECT_EQ(displaced, parked.lock());

  successor.claim.markReady(InitialTrackState{});
}

// Each caller holds its own ticket, so two publishes racing on one thread reclaim
// exactly what they parked, in whatever order their answers arrive.
TEST(LocalForwarderRegistryTest, TicketsAreRedeemedIndependentlyOfOrder) {
  Registry reg;
  auto first = makeForwarder();
  auto second = makeForwarder();
  claimWith(reg, first).markReady(InitialTrackState{});
  auto parkedFirst = reg.replaceAndPark(kFtn, second);
  parkedFirst.claim.markReady(InitialTrackState{});
  auto parkedSecond = reg.replaceAndPark(kFtn, makeForwarder());

  // Out of park order: the second parker's answer arrives first.
  EXPECT_EQ(reg.takeDisplaced(kFtn, std::move(parkedSecond.displaced)), second);
  EXPECT_EQ(reg.takeDisplaced(kFtn, std::move(parkedFirst.displaced)), first);

  parkedSecond.claim.markReady(InitialTrackState{});
}

// Draining is the caller's call, not the registry's: it cannot tell an old publisher that
// owes its subscribers a PUBLISH_DONE from a forwarder the termination cascade will reach.
TEST(LocalForwarderRegistryTest, TakeDisplacedDoesNotTerminateTheForwarder) {
  Registry reg;
  auto first = makeForwarder();
  auto watcher = std::make_shared<DoneWatcher>();
  first->setCallback(watcher);
  claimWith(reg, first).markReady(InitialTrackState{});
  auto successor = reg.replaceAndPark(kFtn, makeForwarder());
  successor.claim.markReady(InitialTrackState{});

  EXPECT_EQ(reg.takeDisplaced(kFtn, std::move(successor.displaced)), first);
  EXPECT_FALSE(watcher->sawPublishDone);
}

// The debt is enforced where it is incurred: dropping the ticket aborts in that scope,
// instead of surfacing later as a registry-teardown abort with no link to the cause.
// Re-seating the occupant would park and install one object at once, leaving the ticket
// pointing at the forwarder still serving the track. Guarded for ready entries as well as
// pending ones, since only the parking form makes it harmful.
TEST(LocalForwarderRegistryDeathTest, ReplaceAndParkWithTheSittingForwarderAborts) {
  EXPECT_DEATH(
      {
        Registry reg;
        auto fwd = makeForwarder();
        claimWith(reg, fwd).markReady(InitialTrackState{});
        auto result = reg.replaceAndPark(kFtn, fwd);
      },
      "re-seats the forwarder that already holds the entry"
  );
}

TEST(LocalForwarderRegistryDeathTest, DroppingAParkTicketAborts) {
  EXPECT_DEATH(
      {
        Registry reg;
        claimWith(reg, makeForwarder()).markReady(InitialTrackState{});
        reg.replaceAndPark(kFtn, makeForwarder()).claim.markReady(InitialTrackState{});
      },
      "park ticket dropped without takeDisplaced\\(\\)"
  );
}

// The registry check is still the backstop: redeeming against the wrong track consumes
// the ticket but leaves the occupant parked, so nothing else can catch it.
TEST(LocalForwarderRegistryDeathTest, DisplacedForwarderOutlivingRegistryAborts) {
  EXPECT_DEATH(
      {
        Registry reg;
        claimWith(reg, makeForwarder()).markReady(InitialTrackState{});
        auto successor = reg.replaceAndPark(kFtn, makeForwarder());
        successor.claim.markReady(InitialTrackState{});
        EXPECT_EQ(reg.takeDisplaced(kOtherFtn, std::move(successor.displaced)), nullptr);
      },
      "displaced forwarder outlived its registry"
  );
}

// === Escape hatch and handle mechanics ===

TEST(LocalForwarderRegistryTest, GetForEvictionReachesAPendingForwarder) {
  Registry reg;
  auto fwd = makeForwarder();
  auto claim = claimWith(reg, fwd);

  EXPECT_EQ(reg.getForEviction(kFtn), fwd);
  EXPECT_EQ(reg.getIfReady(kFtn), nullptr);

  claim.markReady(InitialTrackState{});
}

// Assignment resolves what it overwrites; only the destructor path is otherwise covered.
TEST(LocalForwarderRegistryTest, MoveAssignmentFailsOverwrittenClaim) {
  Registry reg;
  const FullTrackName otherFtn{kTestNs, "track2"};

  auto claim = claimWith(reg, makeForwarder());
  auto waiter = waiterOn(reg);

  auto survivor = std::make_shared<MoQForwarder>(otherFtn);
  auto joined = reg.join(otherFtn, [&] { return survivor; });
  ASSERT_TRUE(std::holds_alternative<Claim>(joined));
  claim = std::move(std::get<Claim>(joined));

  EXPECT_TRUE(settled(std::move(waiter)).hasException());
  EXPECT_TRUE(std::holds_alternative<Absent>(reg.lookup(kFtn)));

  claim.markReady(InitialTrackState{});
  EXPECT_EQ(reg.getIfReady(otherFtn), survivor);
}

TEST(LocalForwarderRegistryTest, MovedFromClaimDoesNotResolve) {
  Registry reg;
  auto fwd = makeForwarder();
  {
    auto original = claimWith(reg, fwd);
    auto moved = std::move(original);
    moved.markReady(InitialTrackState{});
    EXPECT_FALSE(static_cast<bool>(original));
    // ~original must not fail the entry `moved` just marked ready.
  }
  EXPECT_EQ(reg.getIfReady(kFtn), fwd);
}

// A pending entry at teardown means a Claim leaked — otherwise a waiter parked on
// that promise hangs its thread forever. Abort loudly instead.
TEST(LocalForwarderRegistryDeathTest, PendingEntryOutlivingRegistryAborts) {
  EXPECT_DEATH(
      {
        Registry reg;
        auto result = joinWith(reg, makeForwarder());
        new Claim(std::move(std::get<Claim>(result)));
      },
      "pending entry outlived its registry"
  );
}

} // namespace
