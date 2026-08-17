/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/ForwarderRef.h"

#include <folly/Portability.h>
#include <folly/coro/BlockingWait.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>

#include <vector>

using namespace testing;
using namespace moxygen;

namespace {

using openmoq::moqx::ForwarderRef;

const TrackNamespace kTestNs{{"test", "namespace"}};
const FullTrackName kFtn{kTestNs, "track1"};

std::shared_ptr<MoQForwarder> makeForwarder() {
  return std::make_shared<MoQForwarder>(kFtn);
}

// Queues work until drain(), for tests that assert what happens before delivery.
class QueueExecutor : public folly::Executor {
public:
  void add(folly::Func f) override { queue_.push_back(std::move(f)); }
  void drain() {
    auto queue = std::move(queue_);
    for (auto& f : queue) {
      f();
    }
  }

private:
  std::vector<folly::Func> queue_;
};

std::thread::id evbThreadId(folly::ScopedEventBaseThread& evbThread) {
  std::thread::id id;
  evbThread.getEventBase()->runInEventBaseThreadAndWait([&] { id = std::this_thread::get_id(); });
  return id;
}

TEST(ForwarderRefTest, EmptyRefIsEmptyAndExpired) {
  ForwarderRef ref;
  EXPECT_FALSE(static_cast<bool>(ref));
  EXPECT_TRUE(ref.expired());
}

TEST(ForwarderRefTest, OwnedPostRunsInlineOnTheSameForwarder) {
  auto fwd = makeForwarder();
  auto ref = ForwarderRef::owned(fwd, folly::Executor::KeepAlive<>{}, 3);

  const MoQForwarder* seen = nullptr;
  ref.post([&](MoQForwarder& f) { seen = &f; });

  EXPECT_EQ(seen, fwd.get()) << "owned post must run inline, before post() returns";
  EXPECT_FALSE(ref.expired());
  EXPECT_EQ(ref.generation(), 3);
}

TEST(ForwarderRefTest, OwnedCoWithReturnsValueInline) {
  auto fwd = makeForwarder();
  auto ref = ForwarderRef::owned(fwd, folly::Executor::KeepAlive<>{});

  auto result =
      folly::coro::blockingWait(ref.co_with([](MoQForwarder& f) { return f.fullTrackName(); }));

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, kFtn);
}

TEST(ForwarderRefTest, RemotePostRunsOnTheOwnerThread) {
  folly::ScopedEventBaseThread evbThread("owner");
  auto fwd = makeForwarder();
  auto ref =
      ForwarderRef::remote(fwd, folly::getKeepAliveToken(evbThread.getEventBase()), /*gen=*/7);

  std::thread::id ranOn;
  const MoQForwarder* seen = nullptr;
  folly::Baton<> done;
  ref.post([&](MoQForwarder& f) {
    ranOn = std::this_thread::get_id();
    seen = &f;
    done.post();
  });

  ASSERT_TRUE(done.try_wait_for(std::chrono::seconds(5)));
  EXPECT_EQ(ranOn, evbThreadId(evbThread));
  EXPECT_EQ(seen, fwd.get());
}

// Liveness is judged on the owner: a forwarder that dies between post() and
// delivery means fn never runs, rather than fn racing the destruction.
TEST(ForwarderRefTest, RemotePostAfterForwarderDeathIsANoOp) {
  QueueExecutor owner;
  auto fwd = makeForwarder();
  auto ref = ForwarderRef::remote(fwd, folly::getKeepAliveToken(&owner));

  bool ran = false;
  ref.post([&](MoQForwarder&) { ran = true; });
  fwd.reset();
  owner.drain();

  EXPECT_FALSE(ran);
  EXPECT_TRUE(ref.expired());
}

TEST(ForwarderRefTest, RemoteCoWithRoundTripsThroughTheOwnerThread) {
  folly::ScopedEventBaseThread evbThread("owner");
  auto fwd = makeForwarder();
  auto ref = ForwarderRef::remote(fwd, folly::getKeepAliveToken(evbThread.getEventBase()));

  std::thread::id ranOn;
  auto result = folly::coro::blockingWait(ref.co_with([&](MoQForwarder& f) {
    ranOn = std::this_thread::get_id();
    return f.fullTrackName();
  }));

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, kFtn);
  EXPECT_EQ(ranOn, evbThreadId(evbThread));
}

TEST(ForwarderRefTest, RemoteCoWithOnDeadForwarderReturnsNullopt) {
  folly::ScopedEventBaseThread evbThread("owner");
  auto fwd = makeForwarder();
  auto ref = ForwarderRef::remote(fwd, folly::getKeepAliveToken(evbThread.getEventBase()));
  fwd.reset();

  auto result =
      folly::coro::blockingWait(ref.co_with([](MoQForwarder& f) { return f.fullTrackName(); }));

  EXPECT_FALSE(result.has_value());
}

// co_with() must not be a coroutine. A Task is lazy, so a coroutine body would read
// `this` after the ref that produced it is gone — and a temporary at the call site is
// the natural way to use a cheap copyable handle.
TEST(ForwarderRefTest, CoWithOutlivesATemporaryRef) {
  folly::ScopedEventBaseThread evbThread("owner");
  auto fwd = makeForwarder();

  auto readName = [](MoQForwarder& f) { return f.fullTrackName(); };
  auto owned = ForwarderRef::owned(fwd, {}).co_with(readName);
  auto remote = ForwarderRef::remote(fwd, folly::getKeepAliveToken(evbThread.getEventBase()))
                    .co_with(readName);

  // Both refs died at the end of their full-expression, before either Task started.
  auto ownedResult = folly::coro::blockingWait(std::move(owned));
  auto remoteResult = folly::coro::blockingWait(std::move(remote));

  ASSERT_TRUE(ownedResult.has_value());
  EXPECT_EQ(*ownedResult, kFtn);
  ASSERT_TRUE(remoteResult.has_value());
  EXPECT_EQ(*remoteResult, kFtn);
}

TEST(ForwarderRefTest, PinOnOwnerGivesStrongAccess) {
  QueueExecutor owner;
  auto fwd = makeForwarder();
  auto ref = ForwarderRef::remote(fwd, folly::getKeepAliveToken(&owner));

  auto pin = ref.pinOnOwner();
  ASSERT_TRUE(pin.has_value());
  EXPECT_EQ(&**pin, fwd.get());
  EXPECT_EQ((*pin)->fullTrackName(), kFtn);
}

TEST(ForwarderRefTest, PinKeepsForwarderAliveUntilReleased) {
  QueueExecutor owner;
  auto fwd = makeForwarder();
  auto ref = ForwarderRef::remote(fwd, folly::getKeepAliveToken(&owner));

  auto pin = ref.pinOnOwner();
  ASSERT_TRUE(pin.has_value());
  fwd.reset();

  EXPECT_FALSE(ref.expired()) << "the pin holds the last strong ref";
  pin.reset();
  EXPECT_TRUE(ref.expired());
}

TEST(ForwarderRefTest, PinOnDeadRemoteReturnsNullopt) {
  QueueExecutor owner;
  auto fwd = makeForwarder();
  auto ref = ForwarderRef::remote(fwd, folly::getKeepAliveToken(&owner));
  fwd.reset();

  EXPECT_FALSE(ref.pinOnOwner().has_value());
}

TEST(ForwarderRefTest, PinIfOwnedEngagedOnlyForOwned) {
  QueueExecutor owner;
  auto fwd = makeForwarder();
  auto owned = ForwarderRef::owned(fwd, folly::Executor::KeepAlive<>{});
  auto remote = ForwarderRef::remote(fwd, folly::getKeepAliveToken(&owner));

  auto pin = owned.pinIfOwned();
  ASSERT_TRUE(pin.has_value());
  EXPECT_EQ(&**pin, fwd.get());

  EXPECT_FALSE(remote.pinIfOwned().has_value()) << "remote-owned state is unreadable here";
  EXPECT_FALSE(ForwarderRef{}.pinIfOwned().has_value());
}

TEST(ForwarderRefTest, StrongOnOwnerYieldsTheOwnedStrong) {
  auto fwd = makeForwarder();
  auto ref = ForwarderRef::owned(fwd, folly::Executor::KeepAlive<>{});
  EXPECT_EQ(ref.strongOnOwner(), fwd);
}

// The rule the type enforces: remote refs never yield a strong ref, anywhere.
TEST(ForwarderRefDeathTest, StrongOnOwnerOnRemoteAborts) {
  EXPECT_DEATH(
      {
        folly::ScopedEventBaseThread evbThread("owner");
        auto fwd = makeForwarder();
        auto ref = ForwarderRef::remote(fwd, folly::getKeepAliveToken(evbThread.getEventBase()));
        ref.strongOnOwner();
      },
      "strongOnOwner\\(\\) on a non-owned ForwarderRef"
  );
}

TEST(ForwarderRefTest, IdentityKeyMatchesForwarderAndSurvivesDeath) {
  QueueExecutor owner;
  auto fwd = makeForwarder();
  auto ref = ForwarderRef::remote(fwd, folly::getKeepAliveToken(&owner), /*gen=*/4);
  EXPECT_EQ(ref.identityKey().address, fwd.get());
  EXPECT_EQ(ref.identityKey().generation, 4);

  const auto id = ref.identityKey();
  fwd.reset();
  EXPECT_TRUE(ref.expired());
  EXPECT_EQ(ref.identityKey(), id) << "identity must outlive the forwarder";
}

// Two refs to the same address are still distinguishable once the generation differs,
// which is what keeps an identity check honest after an address is recycled.
TEST(ForwarderRefTest, GenerationSeparatesRefsSharingAnAddress) {
  QueueExecutor owner;
  auto fwd = makeForwarder();
  auto first = ForwarderRef::remote(fwd, folly::getKeepAliveToken(&owner), /*gen=*/1);
  auto second = ForwarderRef::remote(fwd, folly::getKeepAliveToken(&owner), /*gen=*/2);

  EXPECT_EQ(first.identityKey().address, second.identityKey().address);
  EXPECT_NE(first.identityKey(), second.identityKey());
}

// A moved-from ref must not keep claiming to be a live owned handle: mode_ and the
// cached address travel by copy under an implicit move, which would leave it
// answering expired()==false over a null strong_.
TEST(ForwarderRefTest, MovedFromRefReadsEmpty) {
  auto fwd = makeForwarder();
  auto ref = ForwarderRef::owned(fwd, {}, /*gen=*/9);
  auto moved = std::move(ref);

  EXPECT_FALSE(static_cast<bool>(ref));
  EXPECT_TRUE(ref.expired());
  EXPECT_FALSE(static_cast<bool>(ref.identityKey()));
  EXPECT_FALSE(ref.pinIfOwned().has_value());

  EXPECT_TRUE(static_cast<bool>(moved));
  EXPECT_EQ(moved.identityKey().address, fwd.get());
  EXPECT_EQ(moved.identityKey().generation, 9);
}

// The point of caching identity by value: relay-side code can name a dead
// publication without any deref.
TEST(ForwarderRefTest, IdentityReadableAfterForwarderDeath) {
  QueueExecutor owner;
  auto fwd = makeForwarder();
  auto ref = ForwarderRef::remote(fwd, folly::getKeepAliveToken(&owner), /*gen=*/42);
  fwd.reset();

  EXPECT_TRUE(static_cast<bool>(ref));
  EXPECT_TRUE(ref.expired());
  EXPECT_EQ(ref.ftn(), kFtn);
  EXPECT_EQ(ref.generation(), 42);
}

TEST(ForwarderRefTest, TrackSurvivesForwarderDeath) {
  QueueExecutor owner;
  auto fwd = makeForwarder();
  auto ref = ForwarderRef::remote(fwd, folly::getKeepAliveToken(&owner));

  auto track = ref.track();
  fwd.reset();

  // The coordinate is what a holder posts with, so it must outlast the forwarder.
  EXPECT_TRUE(ref.expired());
  EXPECT_EQ(track.ftn, kFtn);
  EXPECT_EQ(track.exec, &owner);
}

TEST(ForwarderRefTest, OwnedTrackHasNoExec) {
  auto fwd = makeForwarder();
  auto ref = ForwarderRef::owned(fwd, {});

  EXPECT_EQ(ref.track().ftn, kFtn);
  EXPECT_EQ(ref.track().exec, nullptr);
}

TEST(ForwarderRefTest, MovedPinTransfersTheStrongRef) {
  QueueExecutor owner;
  auto fwd = makeForwarder();
  auto ref = ForwarderRef::remote(fwd, folly::getKeepAliveToken(&owner));

  auto pin = ref.pinOnOwner();
  ASSERT_TRUE(pin.has_value());
  auto moved = std::move(*pin);
  pin.reset(); // moved-from pin must release without complaint

  EXPECT_EQ(moved->fullTrackName(), kFtn);
}

TEST(ForwarderRefDeathTest, PinDereferencedOffThreadAborts) {
  if (!folly::kIsDebug) {
    GTEST_SKIP() << "XDCHECK is compiled out in opt builds";
  }
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
      {
        folly::ScopedEventBaseThread evbThread("owner");
        auto fwd = makeForwarder();
        auto ref = ForwarderRef::remote(fwd, folly::getKeepAliveToken(evbThread.getEventBase()));
        std::optional<ForwarderRef::Pin> escaped;
        evbThread.getEventBase()->runInEventBaseThreadAndWait([&] {
          escaped.emplace(*ref.pinOnOwner());
        });
        (*escaped)->fullTrackName(); // used on the main thread, not the taking thread
      },
      "Pin used off the thread that took it"
  );
}

// The escape hatch this type closes: a Pin that leaves its thread aborts in
// debug instead of silently extending the forwarder's lifetime elsewhere.
TEST(ForwarderRefDeathTest, PinReleasedOffThreadAborts) {
  if (!folly::kIsDebug) {
    GTEST_SKIP() << "XDCHECK is compiled out in opt builds";
  }
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
      {
        folly::ScopedEventBaseThread evbThread("owner");
        auto fwd = makeForwarder();
        auto ref = ForwarderRef::remote(fwd, folly::getKeepAliveToken(evbThread.getEventBase()));
        std::optional<ForwarderRef::Pin> escaped;
        evbThread.getEventBase()->runInEventBaseThreadAndWait([&] {
          escaped.emplace(*ref.pinOnOwner());
        });
        escaped.reset(); // released on the main thread, not the taking thread
      },
      "Pin released off the thread that took it"
  );
}

} // namespace
