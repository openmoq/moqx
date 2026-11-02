/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "relay/LocalForwarderRegistry.h"

#include <functional>
#include <memory>
#include <moxygen/relay/MoQForwarder.h>

namespace openmoq::moqx {

// Runs on the local forwarder's executor. Delegates lifecycle events to
// downstream (a CrossExecForwarderCallback targeting the publisher's executor)
// and owns the forwarder's removal from tlForwarders_.
//
// Removal is identity-checked (only vacates the slot if it still holds THIS
// forwarder), making teardown order-independent: a terminated forwarder can
// never clobber a newer one that has claimed the same track name.
//
// removeOnEmpty distinguishes the two roles:
//   - subscribe-path local forwarder (removeOnEmpty=true): when its last
//     subscriber leaves, its channel sub is pulled from the publisher and it is
//     dead — remove on onEmpty as well as onPublishDone.
//   - publisher's publisher forwarder (removeOnEmpty=false): it must survive
//     subscriber churn (new subscribers may arrive while the publisher is
//     live), so it is removed ONLY when the source terminates (onPublishDone).
class LocalForwarderCallback : public moxygen::MoQForwarder::Callback {
public:
  LocalForwarderCallback(
      LocalForwarderRegistry* localReg,
      moxygen::FullTrackName ftn,
      std::shared_ptr<moxygen::MoQForwarder::Callback> downstream,
      bool removeOnEmpty = true
  )
      : localReg_(localReg), ftn_(std::move(ftn)), downstream_(std::move(downstream)),
        removeOnEmpty_(removeOnEmpty) {}

  void onEmpty(moxygen::MoQForwarder* forwarder) override {
    // Released on return: dropping it can destroy the forwarder and this callback.
    auto hold = std::move(drainHold_);
    downstream_->onEmpty(forwarder);
    if (removeOnEmpty_) {
      localReg_->remove(ftn_, forwarder);
    }
  }

  void onPublishDone(moxygen::MoQForwarder* forwarder) override {
    downstream_->onPublishDone(forwarder);
    // Source-driven teardown (publisher/upstream terminated). Identity-checked.
    auto removed = localReg_->remove(ftn_, forwarder);
    // Keeps a draining forwarder alive until onEmpty, after the session drops its ref.
    if (forwarder->subscriberCount() > 0) {
      drainHold_ = std::move(removed);
    }
  }

  void forwardChanged(moxygen::MoQForwarder* forwarder, bool forward) override {
    downstream_->forwardChanged(forwarder, forward);
  }

  void newGroupRequested(moxygen::MoQForwarder* forwarder, uint64_t group) override {
    downstream_->newGroupRequested(forwarder, group);
  }

  using FillHandler = std::function<void(std::shared_ptr<moxygen::TrackConsumer>, moxygen::Fetch)>;
  void setFillHandler(FillHandler fillHandler) { fillHandler_ = std::move(fillHandler); }

  // Serves inline: beginFill has to run on this executor, and downstream_ runs on another.
  void fillRequested(
      moxygen::MoQForwarder* /*forwarder*/,
      std::shared_ptr<moxygen::TrackConsumer> consumer,
      moxygen::Fetch fetch
  ) override {
    if (fillHandler_) {
      fillHandler_(std::move(consumer), std::move(fetch));
    }
  }

private:
  LocalForwarderRegistry* localReg_;
  moxygen::FullTrackName ftn_;
  std::shared_ptr<moxygen::MoQForwarder::Callback> downstream_;
  bool removeOnEmpty_;
  FillHandler fillHandler_;
  std::shared_ptr<moxygen::MoQForwarder> drainHold_;
};

} // namespace openmoq::moqx
