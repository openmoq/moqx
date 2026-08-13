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

#include <atomic>
#include <memory>
#include <optional>
#include <thread>
#include <type_traits>

namespace openmoq::moqx {

// A track and the executor its forwarder lives on.  The receiver of this ref
// can hop back to exec to get the latest publisher for this track.
struct TrackRef {
  moxygen::FullTrackName ftn;
  folly::Executor* exec{nullptr};
};

// Uniquely identifies a MoQForwarder - even after it dies - by the address of the forwarder and a
// generation number.
struct ForwarderId {
  const void* address{nullptr};
  uint64_t generation{0};

  friend bool operator==(const ForwarderId&, const ForwarderId&) = default;
  explicit operator bool() const { return address != nullptr; }
};

// A reference to a MoQForwarder that may be held on any thread but only used on
// the forwarder's owning executor. This class makes cross-thread dereference and cross-thread
// destruction unrepresentable: there is no get() or operator->, and no operation hands a strong ref
// to a thread other than the owner's.
//
// Two modes, fixed at construction:
//
//   owned    SingleThread/RelayExec: the holder's thread is the owner, so the
//            ref keeps the forwarder alive and every operation runs inline.
//   remote   LocalForwarder: the owning thread's registry keeps the forwarder
//            alive; this holds weak + the owner executor, and every operation
//            travels there as a message.
//
// Call sites are mode-agnostic; the mode decision lives at the one construction
// point per path.
//
// Track name/identity are cached by value, so both stay readable on any thread
// after the forwarder dies — distinguishing "no publication" (empty ref) from
// "publication ended" (remote ref whose forwarder expired).
class ForwarderRef {
public:
  // Control plane only — once per publication, never per object. Not a data-path pattern.
  static uint64_t nextGeneration() {
    static std::atomic<uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
  }

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
      raw_ = std::exchange(other.raw_, nullptr);
      owner_ = std::move(other.owner_);
      gen_ = std::exchange(other.gen_, 0);
      ftn_ = std::move(other.ftn_);
    }
    return *this;
  }

  // Non-LF modes. ownerExec may be empty in SingleThread; the owner is simply
  // the only thread there is.
  static ForwarderRef owned(
      std::shared_ptr<moxygen::MoQForwarder> forwarder,
      folly::Executor::KeepAlive<> ownerExec,
      uint64_t generation = nextGeneration()
  ) {
    XCHECK(forwarder) << "owned() with a null forwarder";
    ForwarderRef ref;
    ref.mode_ = Mode::Owned;
    ref.ftn_ = forwarder->fullTrackName();
    ref.raw_ = forwarder.get();
    ref.strong_ = std::move(forwarder);
    ref.owner_ = std::move(ownerExec);
    ref.gen_ = generation;
    return ref;
  }

  // LF mode. Minted where a strong ref is legitimately in hand, but only the weak survives in the
  // ref.
  static ForwarderRef remote(
      const std::shared_ptr<moxygen::MoQForwarder>& forwarder,
      folly::Executor::KeepAlive<> ownerExec,
      uint64_t generation = nextGeneration()
  ) {
    XCHECK(forwarder) << "remote() with a null forwarder";
    XCHECK(ownerExec) << "remote() with no owner executor";
    ForwarderRef ref;
    ref.mode_ = Mode::Remote;
    ref.ftn_ = forwarder->fullTrackName();
    ref.raw_ = forwarder.get();
    ref.weak_ = forwarder;
    ref.owner_ = std::move(ownerExec);
    ref.gen_ = generation;
    return ref;
  }

  // A publication was referenced, whether or not it is still alive.
  explicit operator bool() const { return mode_ != Mode::Empty; }

  bool expired() const {
    switch (mode_) {
    case Mode::Empty:
      return true;
    case Mode::Owned:
      return false;
    case Mode::Remote:
      return weak_.expired();
    }
    folly::assume_unreachable();
  }

  const moxygen::FullTrackName& ftn() const {
    XCHECK(mode_ != Mode::Empty) << "ftn() on an empty ForwarderRef";
    return ftn_;
  }

  TrackRef track() const {
    XCHECK(mode_ != Mode::Empty) << "track() on an empty ForwarderRef";
    return TrackRef{ftn_, owner_.get()};
  }

  // For identity comparisons only. Valid after death.
  ForwarderId identityKey() const { return ForwarderId{raw_, gen_}; }

  // Separates this publication from a later one that reuses the same address.
  uint64_t generation() const { return gen_; }

  folly::Executor* ownerExec() const { return owner_.get(); }

  // Fire-and-forget on the owner. Remote: enqueued, and fn runs only if the
  // forwarder is still alive when the message arrives — liveness is judged on
  // the owner thread. Owned: inline; those modes' callers are on the owner by construction.
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

  // Strong access for a scope already running on the owner executor. Every
  // dereference and the release are checked against the thread that took the
  // pin, so it cannot smuggle the strong ref off-thread.
  class Pin {
  public:
    Pin(Pin&&) noexcept = default;
    // Move-construction carries takenOn_ along, so the release check still applies.
    // Assignment is deleted.
    Pin& operator=(Pin&&) = delete;
    Pin(const Pin&) = delete;
    Pin& operator=(const Pin&) = delete;
    ~Pin() {
      XDCHECK(!fwd_ || takenOn_ == std::this_thread::get_id())
          << "Pin released off the thread that took it";
    }

    moxygen::MoQForwarder& operator*() const {
      dcheckPinnedThread();
      return *fwd_;
    }
    moxygen::MoQForwarder* operator->() const {
      dcheckPinnedThread();
      return fwd_.get();
    }

  private:
    friend class ForwarderRef;
    explicit Pin(std::shared_ptr<moxygen::MoQForwarder> fwd) : fwd_(std::move(fwd)) {}

    void dcheckPinnedThread() const {
      XDCHECK(fwd_);
      XDCHECK(takenOn_ == std::this_thread::get_id()) << "Pin used off the thread that took it";
    }

    std::shared_ptr<moxygen::MoQForwarder> fwd_;
    std::thread::id takenOn_{std::this_thread::get_id()};
  };

  // Caller must be on the owner executor. nullopt if the forwarder died.
  std::optional<Pin> pinOnOwner() const {
    XCHECK(mode_ != Mode::Empty) << "pinOnOwner() on an empty ForwarderRef";
    if (mode_ == Mode::Owned) {
      return Pin(strong_);
    }
    auto fwd = weak_.lock();
    if (!fwd) {
      return std::nullopt;
    }
    return Pin(std::move(fwd));
  }

  // Engaged only in owned mode, where the holder's thread is the owner and reading
  // inline is legal.
  std::optional<Pin> pinIfOwned() const {
    if (mode_ != Mode::Owned) {
      return std::nullopt;
    }
    return Pin(strong_);
  }

  // Owned mode only: the holder's thread owns the forwarder, so extending into
  // same-thread async frames is legal. Remote refs never yield a strong — use
  // pinOnOwner() inside a sortie, or resolve from the owner thread's registry.
  std::shared_ptr<moxygen::MoQForwarder> strongOnOwner() const {
    XCHECK(mode_ == Mode::Owned) << "strongOnOwner() on a non-owned ForwarderRef";
    return strong_;
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
  const void* raw_{nullptr};                      // identity only, never dereferenced
  folly::Executor::KeepAlive<> owner_;
  uint64_t gen_{0};
  moxygen::FullTrackName ftn_;
};

} // namespace openmoq::moqx
