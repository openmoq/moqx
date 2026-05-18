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
    // and add a MoQCrossExecFilter subscriber to the primary forwarder.
    bool isNew;
  };

  // Returns the existing local forwarder for ftn, or calls factory() to create
  // one. factory() is called at most once per ftn per thread lifetime.
  GetOrCreateResult getOrCreate(
      const moxygen::FullTrackName& ftn,
      folly::FunctionRef<std::shared_ptr<moxygen::MoQForwarder>()> factory) {
    auto it = forwarders_.find(ftn);
    if (it != forwarders_.end()) {
      return {it->second, /*isNew=*/false};
    }
    auto forwarder = factory();
    forwarders_.emplace(ftn, forwarder);
    return {std::move(forwarder), /*isNew=*/true};
  }

  std::shared_ptr<moxygen::MoQForwarder> get(
      const moxygen::FullTrackName& ftn) const {
    auto it = forwarders_.find(ftn);
    return it != forwarders_.end() ? it->second : nullptr;
  }

  // Called from the local forwarder's onEmpty, which fires on this thread.
  void remove(const moxygen::FullTrackName& ftn) {
    forwarders_.erase(ftn);
  }

 private:
  folly::F14FastMap<
      moxygen::FullTrackName,
      std::shared_ptr<moxygen::MoQForwarder>,
      moxygen::FullTrackName::hash>
      forwarders_;
};

} // namespace openmoq::moqx
