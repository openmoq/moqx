/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/ForwarderRef.h"

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

TEST(ForwarderRefTest, EmptyRefIsFalsyAndYieldsNothing) {
  ForwarderRef ref;
  EXPECT_FALSE(static_cast<bool>(ref));
  EXPECT_EQ(ref.getIfOwned(), nullptr);
}

TEST(ForwarderRefTest, OwnedPostRunsInlineOnTheSameForwarder) {
  auto fwd = makeForwarder();
  auto ref = ForwarderRef::owned(fwd);

  const MoQForwarder* seen = nullptr;
  ref.post([&](MoQForwarder& f) { seen = &f; });

  EXPECT_EQ(seen, fwd.get()) << "owned post must run inline, before post() returns";
}

TEST(ForwarderRefTest, OwnedCoWithReturnsValueInline) {
  auto fwd = makeForwarder();
  auto ref = ForwarderRef::owned(fwd);

  auto result =
      folly::coro::blockingWait(ref.co_with([](MoQForwarder& f) { return f.fullTrackName(); }));

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, kFtn);
}

TEST(ForwarderRefTest, RemotePostRunsOnTheOwnerThread) {
  folly::ScopedEventBaseThread evbThread("owner");
  auto fwd = makeForwarder();
  auto ref = ForwarderRef::remote(fwd, folly::getKeepAliveToken(evbThread.getEventBase()));

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
  auto owned = ForwarderRef::owned(fwd).co_with(readName);
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

// The rule the type enforces: a remote ref never hands out a pointer, so relay-exec
// code cannot reach a forwarder that lives on another thread.
TEST(ForwarderRefTest, GetIfOwnedYieldsNothingForRemote) {
  QueueExecutor owner;
  auto fwd = makeForwarder();

  EXPECT_EQ(ForwarderRef::owned(fwd).getIfOwned(), fwd);
  EXPECT_EQ(ForwarderRef::remote(fwd, folly::getKeepAliveToken(&owner)).getIfOwned(), nullptr);
}

// A moved-from ref must not keep claiming to be a live owned handle: mode_ travels by
// copy under an implicit move, which would leave it answering true over a null strong_.
TEST(ForwarderRefTest, MovedFromRefReadsEmpty) {
  auto fwd = makeForwarder();
  auto ref = ForwarderRef::owned(fwd);
  auto moved = std::move(ref);

  EXPECT_FALSE(static_cast<bool>(ref));
  EXPECT_EQ(ref.getIfOwned(), nullptr);

  EXPECT_TRUE(static_cast<bool>(moved));
  EXPECT_EQ(moved.getIfOwned(), fwd);
}

TEST(ForwarderRefTest, TrackSurvivesForwarderDeath) {
  QueueExecutor owner;
  auto fwd = makeForwarder();
  auto ref = ForwarderRef::remote(fwd, folly::getKeepAliveToken(&owner));

  auto track = ref.track();
  fwd.reset();

  // The coordinate is what a holder posts with, so it must outlast the forwarder.
  EXPECT_TRUE(static_cast<bool>(ref));
  EXPECT_EQ(track.ftn, kFtn);
  EXPECT_EQ(track.exec, &owner);
  EXPECT_EQ(ref.track().ftn, kFtn) << "still nameable after the forwarder is gone";
}

TEST(ForwarderRefTest, OwnedTrackHasNoExec) {
  auto fwd = makeForwarder();
  auto ref = ForwarderRef::owned(fwd);

  EXPECT_EQ(ref.track().ftn, kFtn);
  EXPECT_EQ(ref.track().exec, nullptr);
}

} // namespace
