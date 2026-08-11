/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "relay/InitialTrackState.h"

#include <moxygen/MoQLocation.h>
#include <moxygen/relay/MoQForwarder.h>

#include <folly/container/F14Map.h>
#include <folly/futures/SharedPromise.h>
#include <folly/logging/xlog.h>

#include <variant>

namespace openmoq::moqx {

// Thread-local registry mapping FullTrackName → local MoQForwarder on one
// iothread. All methods must be called on the owning thread. One instance per
// iothread, stored in MoqxRelay::tlForwarders_.
//
// An entry is in exactly one of three states:
//
//   absent           no forwarder for this track on this thread
//   pending(fwd, p)  fwd is installed but its largest/extensions are not yet
//                    set from upstream. Attaching now would publish a largest
//                    from before that, which a client reads as a track
//                    restart. Exactly one Claim owns the entry and owes a
//                    resolution.
//   ready(fwd)       the owner declared it attachable
//
// Pending deliberately does not expose the forwarder: only the Claim holder can
// reach one before its initial state is set, so a reader has nothing to check.
class LocalForwarderRegistry {
public:
  struct Absent {};
  struct Pending {
    // Fails if the setup fails, the entry is displaced, or the owner drops its Claim.
    folly::SemiFuture<folly::Unit> ready;
  };
  struct Ready {
    std::shared_ptr<moxygen::MoQForwarder> forwarder;
  };
  using State = std::variant<Absent, Pending, Ready>;

  // Sole handle on a pending entry. Move-only; must not outlive the registry or
  // leave its thread. The destructor fails an unresolved claim, so an abandoned
  // setup wakes its waiters instead of stranding them.
  class Claim {
  public:
    Claim() = default;
    Claim(Claim&& other) noexcept { *this = std::move(other); }
    Claim& operator=(Claim&& other) noexcept {
      if (this != &other) {
        resolveIfEngaged(std::runtime_error("local forwarder claim overwritten"));
        registry_ = std::exchange(other.registry_, nullptr);
        ftn_ = std::move(other.ftn_);
        forwarder_ = std::move(other.forwarder_);
        initialStateSet_ = std::exchange(other.initialStateSet_, false);
      }
      return *this;
    }
    ~Claim() { resolveIfEngaged(std::runtime_error("local forwarder claim dropped")); }

    explicit operator bool() const { return registry_ != nullptr; }

    // Copy this out before resolving if the forwarder is needed afterwards.
    const std::shared_ptr<moxygen::MoQForwarder>& forwarder() const {
      XCHECK(registry_) << "forwarder() on a resolved or empty Claim";
      return forwarder_;
    }

    // Separate from markReady() so a caller with setup to finish can order it before
    // waiters are released.
    void setInitialState(const InitialTrackState& initial) {
      XCHECK(registry_) << "setInitialState() on a resolved or empty Claim";
      initial.applyTo(*forwarder_);
      initialStateSet_ = true;
    }

    void markReady() {
      if (registry_) {
        XCHECK(initialStateSet_) << "markReady() before setInitialState() for " << ftn_;
        registry_->markReady(ftn_, forwarder_.get());
        registry_ = nullptr;
      }
    }

    void markReady(const InitialTrackState& initial) {
      setInitialState(initial);
      markReady();
    }

    // pending → absent: a failed setup must not leave a forwarder behind that reads
    // as ready without ever having had its initial state set.
    void fail(folly::exception_wrapper ew) {
      if (registry_) {
        registry_->fail(ftn_, forwarder_.get(), std::move(ew));
        registry_ = nullptr;
      }
    }

  private:
    friend class LocalForwarderRegistry;

    Claim(
        LocalForwarderRegistry* registry,
        moxygen::FullTrackName ftn,
        std::shared_ptr<moxygen::MoQForwarder> forwarder
    )
        : registry_(registry), ftn_(std::move(ftn)), forwarder_(std::move(forwarder)) {}

    void resolveIfEngaged(folly::exception_wrapper ew) noexcept {
      if (registry_) {
        registry_->fail(ftn_, forwarder_.get(), std::move(ew));
        registry_ = nullptr;
      }
    }

    LocalForwarderRegistry* registry_{nullptr};
    moxygen::FullTrackName ftn_;
    std::shared_ptr<moxygen::MoQForwarder> forwarder_;
    bool initialStateSet_{false};
  };

  // Engaged Claim ⇒ this call created the entry and owns setup. After awaiting a
  // Pending, look up again rather than reusing a handle from before the suspend:
  // the entry may have been displaced.
  using JoinResult = std::variant<Claim, Pending, Ready>;

  LocalForwarderRegistry() = default;

  ~LocalForwarderRegistry() {
    for (const auto& [ftn, entry] : forwarders_) {
      XCHECK(!entry.ready) << "pending entry outlived its registry: " << ftn;
    }
  }

  LocalForwarderRegistry(const LocalForwarderRegistry&) = delete;
  LocalForwarderRegistry& operator=(const LocalForwarderRegistry&) = delete;

  // factory() runs only when the entry is absent, at most once per entry lifetime.
  JoinResult join(
      const moxygen::FullTrackName& ftn,
      folly::FunctionRef<std::shared_ptr<moxygen::MoQForwarder>()> factory
  ) {
    if (auto it = forwarders_.find(ftn); it != forwarders_.end()) {
      if (it->second.ready) {
        return Pending{it->second.ready->getSemiFuture()};
      }
      return Ready{it->second.forwarder};
    }
    auto forwarder = factory();
    XCHECK(forwarder) << "join() factory returned null for " << ftn;
    return makePending(ftn, std::move(forwarder));
  }

  // The publisher's forwarder is authoritative for a track on its thread; the
  // displaced one drains itself via the source-termination cascade, and its
  // identity-checked removal then no-ops.
  //
  // A displaced entry's waiters are failed rather than inherited: releasing them
  // onto a forwarder that no longer owns the entry is the restart this type exists
  // to prevent.
  Claim
  replace(const moxygen::FullTrackName& ftn, std::shared_ptr<moxygen::MoQForwarder> forwarder) {
    XCHECK(forwarder) << "replace() with a null forwarder for " << ftn;
    if (auto it = forwarders_.find(ftn); it != forwarders_.end() && it->second.ready) {
      XCHECK(it->second.forwarder != forwarder)
          << "replace() re-seats a forwarder that already owns a pending entry: " << ftn;
      it->second.ready->setException(std::runtime_error("local forwarder displaced"));
    }
    return makePending(ftn, std::move(forwarder));
  }

  // A pending entry reads as absent — see the class comment.
  std::shared_ptr<moxygen::MoQForwarder> getIfReady(const moxygen::FullTrackName& ftn) const {
    auto it = forwarders_.find(ftn);
    return (it != forwarders_.end() && !it->second.ready) ? it->second.forwarder : nullptr;
  }

  State lookup(const moxygen::FullTrackName& ftn) const {
    auto it = forwarders_.find(ftn);
    return it != forwarders_.end() ? stateOf(it->second) : State{Absent{}};
  }

  // The one sanctioned way to reach a forwarder without holding its Claim before it is
  // ready: the caller is tearing it down rather than attaching to it. Every other reader
  // uses getIfReady/lookup.
  std::shared_ptr<moxygen::MoQForwarder> getForEviction(const moxygen::FullTrackName& ftn) const {
    auto it = forwarders_.find(ftn);
    return it != forwarders_.end() ? it->second.forwarder : nullptr;
  }

  // Called from a forwarder's onPublishDone, which fires on this thread. The identity
  // check makes teardown order-independent: a terminated forwarder can only vacate its
  // own entry, so it never clobbers a newer forwarder that has since claimed the same
  // track name.
  void remove(const moxygen::FullTrackName& ftn, const moxygen::MoQForwarder* expected) {
    auto it = forwarders_.find(ftn);
    if (it == forwarders_.end() || it->second.forwarder.get() != expected) {
      return;
    }
    // Erasing destroys the promise, so anyone parked on it must be woken first.
    if (it->second.ready) {
      it->second.ready->setException(std::runtime_error("local forwarder removed"));
    }
    forwarders_.erase(it);
  }

private:
  struct Entry {
    std::shared_ptr<moxygen::MoQForwarder> forwarder;
    // Non-null iff the entry is pending; it is the state discriminator.
    std::shared_ptr<folly::SharedPromise<folly::Unit>> ready;
  };

  static State stateOf(const Entry& entry) {
    if (entry.ready) {
      return Pending{entry.ready->getSemiFuture()};
    }
    return Ready{entry.forwarder};
  }

  Claim
  makePending(const moxygen::FullTrackName& ftn, std::shared_ptr<moxygen::MoQForwarder> forwarder) {
    auto& entry = forwarders_[ftn];
    // Outlive the entry update: dropping the last ref here would run ~MoQForwarder,
    // which reenters the registry through its callback, against a half-written entry.
    auto displaced = std::exchange(entry.forwarder, forwarder);
    entry.ready = std::make_shared<folly::SharedPromise<folly::Unit>>();
    return Claim(this, ftn, std::move(forwarder));
  }

  // A pending entry's promise is unfulfilled by construction: markReady and fail both
  // clear the entry, so neither can run twice against the same promise. The
  // identity check is what makes that hold across displacement — a stale owner
  // finds an entry that is no longer its own and does nothing.
  void markReady(const moxygen::FullTrackName& ftn, const moxygen::MoQForwarder* owner) {
    auto it = findPendingOwnedBy(ftn, owner);
    if (it == forwarders_.end()) {
      return;
    }
    it->second.ready->setValue();
    it->second.ready.reset();
  }

  void fail(
      const moxygen::FullTrackName& ftn,
      const moxygen::MoQForwarder* owner,
      folly::exception_wrapper ew
  ) {
    auto it = findPendingOwnedBy(ftn, owner);
    if (it == forwarders_.end()) {
      return;
    }
    it->second.ready->setException(std::move(ew));
    forwarders_.erase(it);
  }

  using Map = folly::F14FastMap<moxygen::FullTrackName, Entry, moxygen::FullTrackName::hash>;

  // end() unless ftn's entry is pending and still holds `owner`.
  Map::iterator
  findPendingOwnedBy(const moxygen::FullTrackName& ftn, const moxygen::MoQForwarder* owner) {
    auto it = forwarders_.find(ftn);
    if (it == forwarders_.end() || !it->second.ready || it->second.forwarder.get() != owner) {
      return forwarders_.end();
    }
    return it;
  }

  Map forwarders_;
};

} // namespace openmoq::moqx
