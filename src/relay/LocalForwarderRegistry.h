/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/MoQLocation.h>
#include <moxygen/relay/MoQForwarder.h>

#include <folly/container/F14Map.h>
#include <folly/futures/SharedPromise.h>

namespace openmoq::moqx {

// Thread-local registry mapping FullTrackName → local MoQForwarder on one
// iothread. All methods must be called on the owning thread. One instance per
// iothread, stored in MoqxRelay::tlForwarders_.
class LocalForwarderRegistry {
public:
  struct GetOrCreateResult {
    std::shared_ptr<moxygen::MoQForwarder> localForwarder;
    // true only on first creation; caller must cross-exec to publisher thread
    // and add a CrossExecFilter subscriber to the publisher forwarder.
    bool isNew;
  };

  // ready resolves when the forwarder is safe for subscribers to attach to — not that
  // largest is known, since SUBSCRIBE_OK and PUBLISH both carry it optionally. Non-null
  // only while an isNew=true owner has promised to fulfill it.
  struct Entry {
    std::shared_ptr<moxygen::MoQForwarder> forwarder;
    std::shared_ptr<folly::SharedPromise<folly::Unit>> ready;
  };

  // Returns the existing local forwarder for ftn, or calls factory() to create
  // one. factory() is called at most once per ftn per thread lifetime.
  // ready=false arms the gate: an isNew=true caller owns setup and must fulfill
  // ready(ftn) on every exit, or attachers hang.
  GetOrCreateResult getOrCreate(
      const moxygen::FullTrackName& ftn,
      folly::FunctionRef<std::shared_ptr<moxygen::MoQForwarder>()> factory,
      bool ready
  ) {
    auto it = forwarders_.find(ftn);
    if (it != forwarders_.end()) {
      return {it->second.forwarder, /*isNew=*/false};
    }
    auto forwarder = factory();
    auto& entry = forwarders_[ftn];
    entry.forwarder = forwarder;
    if (!ready) {
      entry.ready = std::make_shared<folly::SharedPromise<folly::Unit>>();
    }
    return {std::move(forwarder), /*isNew=*/true};
  }

  // Claim the slot for ftn unconditionally, returning the previous occupant (if
  // any). The publisher's forwarder is authoritative for a track on its thread:
  // a stale subscribe-path local forwarder under the same name is displaced
  // here, and drains itself via the source-termination cascade (its identity-
  // checked removal then no-ops, since this forwarder now owns the slot).
  // ready=true releases attachers now; false leaves them pending until the owner fulfills.
  std::shared_ptr<moxygen::MoQForwarder>
  set(const moxygen::FullTrackName& ftn,
      std::shared_ptr<moxygen::MoQForwarder> forwarder,
      bool ready) {
    auto& entry = forwarders_[ftn];
    auto prev = std::move(entry.forwarder);
    entry.forwarder = std::move(forwarder);
    if (ready && entry.ready && !entry.ready->isFulfilled()) {
      entry.ready->setValue();
    }
    return prev;
  }

  std::shared_ptr<moxygen::MoQForwarder> get(const moxygen::FullTrackName& ftn) const {
    auto it = forwarders_.find(ftn);
    return it != forwarders_.end() ? it->second.forwarder : nullptr;
  }

  // Identity-checked removal: erase the entry for ftn only if it still points
  // at `expected`. Called from a forwarder's onPublishDone, which fires on this
  // thread. The identity check makes teardown order-independent: a terminated
  // forwarder can only vacate its own slot, so it never clobbers a newer
  // forwarder that has since claimed the same track name.
  void remove(const moxygen::FullTrackName& ftn, const moxygen::MoQForwarder* expected) {
    auto it = forwarders_.find(ftn);
    if (it == forwarders_.end() || it->second.forwarder.get() != expected) {
      return;
    }
    forwarders_.erase(it);
  }

  // isNew=false attachers await this, so they never read a largest the owner has not
  // seeded yet — a client sees that as a track restart. Null when no setup is in
  // flight; only getOrCreate(ready=false) arms it.
  std::shared_ptr<folly::SharedPromise<folly::Unit>> ready(const moxygen::FullTrackName& ftn
  ) const {
    auto it = forwarders_.find(ftn);
    return it != forwarders_.end() ? it->second.ready : nullptr;
  }

private:
  folly::F14FastMap<moxygen::FullTrackName, Entry, moxygen::FullTrackName::hash> forwarders_;
};

} // namespace openmoq::moqx
