/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "relay/TrackEventCallback.h"

#include <folly/Executor.h>
#include <memory>
#include <moxygen/relay/MoQForwarder.h>

namespace openmoq::moqx {

// Dispatches MoQForwarder::Callback methods to a target executor (fire-and-forget).
// Reads the track name on the calling thread (the forwarder's owner) and sends that
// across threads, so no reference to the forwarder can be released off its owner.
class CrossExecForwarderCallback final : public moxygen::MoQForwarder::Callback {
public:
  CrossExecForwarderCallback(
      folly::Executor* targetExec,
      std::shared_ptr<TrackEventCallback> downstream
  )
      : targetExec_(targetExec), downstream_(std::move(downstream)) {}

  void onEmpty(moxygen::MoQForwarder* forwarder) override;
  void onPublishDone(moxygen::MoQForwarder* forwarder) override;
  void forwardChanged(moxygen::MoQForwarder* forwarder, bool forward) override;
  void newGroupRequested(moxygen::MoQForwarder* forwarder, uint64_t group) override;

private:
  folly::Executor* targetExec_;
  std::shared_ptr<TrackEventCallback> downstream_;
};

} // namespace openmoq::moqx
