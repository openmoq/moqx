/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/ExecBound.h"

#include <folly/coro/Baton.h>
#include <folly/coro/BlockingWait.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/portability/GTest.h>
#include <moxygen/events/MoQFollyExecutorImpl.h>

#include <thread>

using namespace openmoq::moqx;

namespace {

// Records the thread its destructor ran on.
struct Tracker {
  explicit Tracker(std::thread::id* destroyedOn) : destroyedOn(destroyedOn) {}
  ~Tracker() { *destroyedOn = std::this_thread::get_id(); }
  std::thread::id* destroyedOn;
};

class ExecBoundTest : public ::testing::Test {
protected:
  std::thread::id ownerThread() {
    std::thread::id id;
    owner_.getEventBase()->runInEventBaseThreadAndWait([&] { id = std::this_thread::get_id(); });
    return id;
  }

  void drainOwner() {
    owner_.getEventBase()->runInEventBaseThreadAndWait([] {});
  }

  folly::ScopedEventBaseThread owner_{"owner"};
  moxygen::MoQFollyExecutorImpl exec_{owner_.getEventBase()};
};

TEST_F(ExecBoundTest, ReleaseOffItsExecutorRunsThere) {
  std::thread::id destroyedOn;
  {
    ExecBound<std::shared_ptr<Tracker>> bound(std::make_shared<Tracker>(&destroyedOn), &exec_);
  }
  drainOwner();
  EXPECT_EQ(destroyedOn, ownerThread());
}

TEST_F(ExecBoundTest, ReleaseOnItsExecutorIsInline) {
  std::thread::id destroyedOn;
  bool releasedInline = false;
  owner_.getEventBase()->runInEventBaseThreadAndWait([&] {
    {
      ExecBound<std::shared_ptr<Tracker>> bound(std::make_shared<Tracker>(&destroyedOn), &exec_);
    }
    releasedInline = destroyedOn == std::this_thread::get_id();
  });
  EXPECT_TRUE(releasedInline);
}

TEST_F(ExecBoundTest, PostRunsOnItsExecutor) {
  std::thread::id destroyedOn;
  std::thread::id ranOn;
  {
    ExecBound<std::shared_ptr<Tracker>> bound(std::make_shared<Tracker>(&destroyedOn), &exec_);
    bound.post([&](const std::shared_ptr<Tracker>&) { ranOn = std::this_thread::get_id(); });
  }
  drainOwner();
  EXPECT_EQ(ranOn, ownerThread());
}

TEST_F(ExecBoundTest, PostOnItsExecutorRunsInline) {
  std::thread::id destroyedOn;
  bool ranInline = false;
  owner_.getEventBase()->runInEventBaseThreadAndWait([&] {
    ExecBound<std::shared_ptr<Tracker>> bound(std::make_shared<Tracker>(&destroyedOn), &exec_);
    bool ran = false;
    bound.post([&](const std::shared_ptr<Tracker>&) { ran = true; });
    ranInline = ran;
  });
  EXPECT_TRUE(ranInline);
}

TEST_F(ExecBoundTest, AddQueuesEvenOnItsExecutor) {
  std::thread::id destroyedOn;
  bool ranInline = true;
  bool ran = false;
  owner_.getEventBase()->runInEventBaseThreadAndWait([&] {
    ExecBound<std::shared_ptr<Tracker>> bound(std::make_shared<Tracker>(&destroyedOn), &exec_);
    bound.add([&](const std::shared_ptr<Tracker>&) { ran = true; });
    ranInline = ran;
  });
  drainOwner();
  EXPECT_FALSE(ranInline);
  EXPECT_TRUE(ran);
}

TEST_F(ExecBoundTest, PostMayReleaseTheBoundValue) {
  std::thread::id destroyedOn;
  bool aliveAfterReset = false;
  owner_.getEventBase()->runInEventBaseThreadAndWait([&] {
    ExecBound<std::shared_ptr<Tracker>> bound(std::make_shared<Tracker>(&destroyedOn), &exec_);
    bound.post([&](const std::shared_ptr<Tracker>& tracker) {
      bound.reset();
      aliveAfterReset = tracker && destroyedOn == std::thread::id();
    });
  });
  EXPECT_TRUE(aliveAfterReset);
}

TEST_F(ExecBoundTest, LaunchRunsTheTaskOnItsExecutor) {
  std::thread::id destroyedOn;
  std::thread::id ranOn;
  {
    ExecBound<std::shared_ptr<Tracker>> bound(std::make_shared<Tracker>(&destroyedOn), &exec_);
    bound.launch([&](const std::shared_ptr<Tracker>&) -> folly::coro::Task<void> {
      ranOn = std::this_thread::get_id();
      co_return;
    });
  }
  drainOwner();
  EXPECT_EQ(ranOn, ownerThread());
}

TEST_F(ExecBoundTest, RunReturnsTheTaskResultFromItsExecutor) {
  std::thread::id destroyedOn;
  std::thread::id ranOn;
  {
    ExecBound<std::shared_ptr<Tracker>> bound(std::make_shared<Tracker>(&destroyedOn), &exec_);
    auto result = folly::coro::blockingWait(
        bound.run([&](const std::shared_ptr<Tracker>&) -> folly::coro::Task<int> {
          ranOn = std::this_thread::get_id();
          co_return 7;
        })
    );
    EXPECT_EQ(result, 7);
  }
  drainOwner();
  EXPECT_EQ(ranOn, ownerThread());
}

// The task keeps its own copy after the ExecBound resets, and dropping the task off the executor
// still releases that copy there.
TEST_F(ExecBoundTest, RunTaskReleasesItsCopyOnItsExecutor) {
  std::thread::id destroyedOn;
  ExecBound<std::shared_ptr<Tracker>> bound(std::make_shared<Tracker>(&destroyedOn), &exec_);
  auto task =
      bound.run([](const std::shared_ptr<Tracker>&) -> folly::coro::Task<void> { co_return; });
  bound.reset();
  drainOwner();
  EXPECT_EQ(destroyedOn, std::thread::id()) << "the task's copy is still alive";

  {
    auto dropped = std::move(task);
  }
  drainOwner();
  EXPECT_EQ(destroyedOn, ownerThread());
}

// A task suspended past a reset still reads the value when it resumes.
TEST_F(ExecBoundTest, RunTaskReadsTheValueAfterResumingPastAReset) {
  std::thread::id destroyedOn;
  ExecBound<std::shared_ptr<Tracker>> bound(std::make_shared<Tracker>(&destroyedOn), &exec_);
  folly::coro::Baton resume;
  bool sawValue = false;
  auto done = bound
                  .run([&](const std::shared_ptr<Tracker>& tracker) -> folly::coro::Task<void> {
                    co_await resume;
                    sawValue = tracker && tracker->destroyedOn == &destroyedOn;
                  })
                  .start();
  drainOwner();
  bound.reset();
  drainOwner();
  EXPECT_EQ(destroyedOn, std::thread::id()) << "the suspended task's copy is still alive";

  resume.post();
  std::move(done).get();
  drainOwner();
  EXPECT_TRUE(sawValue);
  EXPECT_EQ(destroyedOn, ownerThread());
}

TEST_F(ExecBoundTest, MoveAssignmentReleasesTheOldValueOnItsExecutor) {
  std::thread::id firstDestroyedOn;
  std::thread::id secondDestroyedOn;
  {
    ExecBound<std::shared_ptr<Tracker>> bound(std::make_shared<Tracker>(&firstDestroyedOn), &exec_);
    bound = {std::make_shared<Tracker>(&secondDestroyedOn), &exec_};
    drainOwner();
    EXPECT_EQ(firstDestroyedOn, ownerThread());
    EXPECT_EQ(secondDestroyedOn, std::thread::id());
  }
  drainOwner();
}

} // namespace
