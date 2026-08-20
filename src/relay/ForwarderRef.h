/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/relay/MoQForwarder.h>

#include <folly/Executor.h>
#include <folly/coro/Task.h>
#include <folly/logging/xlog.h>

#include <memory>
#include <optional>
#include <type_traits>

namespace openmoq::moqx {

// A track and the executor its forwarder lives on.  The receiver of this ref
// can hop back to exec to get the latest publisher for this track.
struct TrackRef {
  moxygen::FullTrackName ftn;
  folly::Executor* exec{nullptr};
};

// A reference to a MoQForwarder that may be held on any thread.  Two modes, fixed
// at construction:
//
//   owned    SingleThread/RelayExec: one executor owns every forwarder and the
//            holder runs on it, so the forwarder is available synchronously.
//   remote   LocalForwarder: the forwarder lives on another executor, whose
//            registry keeps it alive.  Holds weak + that executor, and never yields
//            a pointer: post() and co_with() carry the work there rather than
//            bringing the forwarder here.
//
// getIfOwned() returns null for a remote ref, so a caller on the relay executor
// cannot reach a forwarder that lives elsewhere.  Code already running on the owner
// holds its own weak_ptr rather than asking a ref to hand one over.
//
// The track name and owning executor are cached by value, so track() still answers
// after the forwarder dies and a holder can still name what it was pointed at.
class ForwarderRef {
public:
  ForwarderRef() = default;

  ForwarderRef(const ForwarderRef&) = default;
  ForwarderRef& operator=(const ForwarderRef&) = default;

  // Leaves the source empty
  ForwarderRef(ForwarderRef&& other) noexcept { *this = std::move(other); }
  ForwarderRef& operator=(ForwarderRef&& other) noexcept {
    if (this != &other) {
      mode_ = std::exchange(other.mode_, Mode::Empty);
      strong_ = std::move(other.strong_);
      weak_ = std::move(other.weak_);
      owner_ = std::move(other.owner_);
      ftn_ = std::move(other.ftn_);
    }
    return *this;
  }

  // Non-LF modes, where the caller's executor is the only one that owns forwarders.
  static ForwarderRef owned(std::shared_ptr<moxygen::MoQForwarder> forwarder) {
    XCHECK(forwarder) << "owned() with a null forwarder";
    ForwarderRef ref;
    ref.mode_ = Mode::Owned;
    ref.ftn_ = forwarder->fullTrackName();
    ref.strong_ = std::move(forwarder);
    return ref;
  }

  // LF mode. Minted where a strong ref is legitimately in hand, but only the weak survives in the
  // ref.
  static ForwarderRef remote(
      const std::shared_ptr<moxygen::MoQForwarder>& forwarder,
      folly::Executor::KeepAlive<> ownerExec
  ) {
    XCHECK(forwarder) << "remote() with a null forwarder";
    XCHECK(ownerExec) << "remote() with no owner executor";
    ForwarderRef ref;
    ref.mode_ = Mode::Remote;
    ref.ftn_ = forwarder->fullTrackName();
    ref.weak_ = forwarder;
    ref.owner_ = std::move(ownerExec);
    return ref;
  }

  // A publication was referenced, whether or not it is still alive.
  explicit operator bool() const { return mode_ != Mode::Empty; }

  TrackRef track() const {
    XCHECK(mode_ != Mode::Empty) << "track() on an empty ForwarderRef";
    return TrackRef{ftn_, owner_.get()};
  }

  // Null in owned mode, where the holder is already on the owning executor.
  folly::Executor* ownerExec() const { return owner_.get(); }

  // Null unless this is an owned ref, where the caller owns the forwarder by
  // construction.  Refuses in remote mode so a relay-exec caller cannot reach a
  // forwarder that lives elsewhere.
  std::shared_ptr<moxygen::MoQForwarder> getIfOwned() const { return strong_; }

  // Fire-and-forget on the owner. Remote: enqueued, and fn runs only if the
  // forwarder is still alive when the message arrives — liveness is judged on
  // the owner thread. Owned: inline.
  void post(folly::Function<void(moxygen::MoQForwarder&)> fn) const {
    XCHECK(mode_ != Mode::Empty) << "post() on an empty ForwarderRef";
    if (mode_ == Mode::Owned) {
      fn(*strong_);
      return;
    }
    owner_->add([weak = weak_, fn = std::move(fn)]() mutable {
      if (auto fwd = weak.lock()) {
        fn(*fwd);
      }
    });
  }

  // Round trip to the owner; nullopt if the forwarder died first. Not a coroutine -- it returns an
  // awaitable Task. Every read of the ref happens now, and the callee's frame owns everything the
  // Task touches later.
  template <class Fn>
  auto co_with(Fn fn) const
      -> folly::coro::Task<std::optional<std::invoke_result_t<Fn&, moxygen::MoQForwarder&>>> {
    using R = std::invoke_result_t<Fn&, moxygen::MoQForwarder&>;
    static_assert(!std::is_void_v<R>, "co_with() returns a value; use post() for fire-and-forget");
    XCHECK(mode_ != Mode::Empty) << "co_with() on an empty ForwarderRef";
    if (mode_ == Mode::Owned) {
      return runInline<Fn, R>(strong_, std::move(fn));
    }
    return lockAndRunOn<Fn, R>(owner_.copy(), weak_, std::move(fn));
  }

private:
  enum class Mode { Empty, Owned, Remote };

  template <class Fn, class R>
  static folly::coro::Task<std::optional<R>>
  lockAndRun(std::weak_ptr<moxygen::MoQForwarder> weak, Fn fn) {
    auto fwd = weak.lock();
    if (!fwd) {
      co_return std::nullopt;
    }
    co_return fn(*fwd);
  }

  // Owns the hop as well as the lock, so co_with() need not be a coroutine.
  template <class Fn, class R>
  static folly::coro::Task<std::optional<R>> lockAndRunOn(
      folly::Executor::KeepAlive<> owner,
      std::weak_ptr<moxygen::MoQForwarder> weak,
      Fn fn
  ) {
    co_return co_await folly::coro::co_withExecutor(
        std::move(owner),
        lockAndRun<Fn, R>(std::move(weak), std::move(fn))
    );
  }

  // The holder's thread is the owner, so there is no hop, but the strong ref still has to move into
  // a frame the Task owns.
  template <class Fn, class R>
  static folly::coro::Task<std::optional<R>>
  runInline(std::shared_ptr<moxygen::MoQForwarder> fwd, Fn fn) {
    co_return fn(*fwd);
  }

  Mode mode_{Mode::Empty};
  std::shared_ptr<moxygen::MoQForwarder> strong_; // Owned only
  std::weak_ptr<moxygen::MoQForwarder> weak_;     // Remote only
  folly::Executor::KeepAlive<> owner_;            // Remote only
  moxygen::FullTrackName ftn_;
};

} // namespace openmoq::moqx
