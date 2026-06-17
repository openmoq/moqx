/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/MoQLocation.h>
#include <moxygen/relay/MoQForwarder.h>

#include <folly/container/F14Map.h>

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

  // Returns the existing local forwarder for ftn, or calls factory() to create
  // one. factory() is called at most once per ftn per thread lifetime.
  GetOrCreateResult getOrCreate(
      const moxygen::FullTrackName& ftn,
      folly::FunctionRef<std::shared_ptr<moxygen::MoQForwarder>()> factory
  ) {
    auto it = forwarders_.find(ftn);
    if (it != forwarders_.end()) {
      return {it->second, /*isNew=*/false};
    }
    auto forwarder = factory();
    forwarders_.emplace(ftn, forwarder);
    return {std::move(forwarder), /*isNew=*/true};
  }

  // Claim the slot for ftn unconditionally, returning the previous occupant (if
  // any). The publisher's forwarder is authoritative for a track on its thread:
  // a stale subscribe-path local forwarder under the same name is displaced
  // here, and drains itself via the source-termination cascade (its identity-
  // checked removal then no-ops, since this forwarder now owns the slot).
  std::shared_ptr<moxygen::MoQForwarder>
  set(const moxygen::FullTrackName& ftn, std::shared_ptr<moxygen::MoQForwarder> forwarder) {
    auto prev = std::move(forwarders_[ftn]);
    forwarders_[ftn] = std::move(forwarder);
    return prev;
  }

  std::shared_ptr<moxygen::MoQForwarder> get(const moxygen::FullTrackName& ftn) const {
    auto it = forwarders_.find(ftn);
    return it != forwarders_.end() ? it->second : nullptr;
  }

  // Identity-checked removal: erase the entry for ftn only if it still points
  // at `expected`. Called from a forwarder's onPublishDone, which fires on this
  // thread. The identity check makes teardown order-independent: a terminated
  // forwarder can only vacate its own slot, so it never clobbers a newer
  // forwarder that has since claimed the same track name.
  void remove(const moxygen::FullTrackName& ftn, const moxygen::MoQForwarder* expected) {
    auto it = forwarders_.find(ftn);
    if (it == forwarders_.end() || it->second.get() != expected) {
      return;
    }
    forwarders_.erase(it);
  }

private:
  folly::F14FastMap<
      moxygen::FullTrackName,
      std::shared_ptr<moxygen::MoQForwarder>,
      moxygen::FullTrackName::hash>
      forwarders_;
};

} // namespace openmoq::moqx
