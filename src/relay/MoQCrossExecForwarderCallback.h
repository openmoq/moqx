/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/Executor.h>
#include <memory>
#include <moxygen/relay/MoQForwarder.h>

namespace openmoq::moqx {

// Dispatches MoQForwarder::Callback methods to a target executor (fire-and-forget).
// Captures FullTrackName by value on the calling thread to avoid dereferencing the
// raw MoQForwarder* across the executor hop. On the target executor the forwarder is
// recovered via a weak_ptr; if it has been destroyed the callback is skipped.
class MoQCrossExecForwarderCallback final : public moxygen::MoQForwarder::Callback {
public:
  MoQCrossExecForwarderCallback(
      folly::Executor* targetExec,
      std::weak_ptr<moxygen::MoQForwarder> forwarder,
      std::shared_ptr<moxygen::MoQForwarder::Callback> downstream)
      : targetExec_(targetExec),
        forwarder_(std::move(forwarder)),
        downstream_(std::move(downstream)) {}

  void onEmpty(moxygen::MoQForwarder* forwarder) override;
  void forwardChanged(moxygen::MoQForwarder* forwarder, bool forward) override;
  void newGroupRequested(moxygen::MoQForwarder* forwarder, uint64_t group) override;

private:
  folly::Executor* targetExec_;
  std::weak_ptr<moxygen::MoQForwarder> forwarder_;
  std::shared_ptr<moxygen::MoQForwarder::Callback> downstream_;
};

} // namespace openmoq::moqx
