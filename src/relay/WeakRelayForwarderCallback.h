/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <moxygen/relay/MoQForwarder.h>

namespace openmoq::moqx {

// Relay-side MoQForwarder::Callback target. Holds a weak_ptr to the relay
// (as its MoQForwarder::Callback base) to avoid a reference cycle
// (registry → forwarder → callback → relay). Intended to be used as the
// downstream of CrossExecForwarderCallback, which handles cross-exec
// dispatch and forwarder pointer recovery.
class WeakRelayForwarderCallback final : public moxygen::MoQForwarder::Callback {
public:
  explicit WeakRelayForwarderCallback(std::weak_ptr<moxygen::MoQForwarder::Callback> relay)
      : relay_(std::move(relay)) {}

  void onEmpty(moxygen::MoQForwarder* forwarder) override;
  void forwardChanged(moxygen::MoQForwarder* forwarder, bool forward) override;
  void newGroupRequested(moxygen::MoQForwarder* forwarder, uint64_t group) override;

private:
  std::weak_ptr<moxygen::MoQForwarder::Callback> relay_;
};

} // namespace openmoq::moqx
