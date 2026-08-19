/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "relay/TrackEventCallback.h"

#include <memory>
#include <moxygen/relay/MoQForwarder.h>

namespace openmoq::moqx {

class MoqxRelay;

// Relay-side sink for forwarder lifecycle events. Holds a weak_ptr to the relay to
// avoid the cycle registry → forwarder → callback → relay.
//
// Installed directly on the forwarder in SingleThread/RelayExec and used as the
// downstream of CrossExecForwarderCallback in LocalForwarder mode. The
// MoQForwarder::Callback overrides only extract the track name and delegate, so both paths
// run identical logic.
class WeakRelayForwarderCallback final : public moxygen::MoQForwarder::Callback,
                                         public TrackEventCallback {
public:
  explicit WeakRelayForwarderCallback(std::weak_ptr<MoqxRelay> relay) : relay_(std::move(relay)) {}

  void onEmpty(moxygen::MoQForwarder* forwarder) override;
  void forwardChanged(moxygen::MoQForwarder* forwarder, bool forward) override;
  void newGroupRequested(moxygen::MoQForwarder* forwarder, uint64_t group) override;

  void onEmpty(const moxygen::FullTrackName& ftn) override;
  void forwardChanged(const moxygen::FullTrackName& ftn, bool forward) override;
  void newGroupRequested(const moxygen::FullTrackName& ftn, uint64_t group) override;
  void onPublishDone(const moxygen::FullTrackName& ftn) override;

private:
  std::weak_ptr<MoqxRelay> relay_;
};

} // namespace openmoq::moqx
