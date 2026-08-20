/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "WeakRelayForwarderCallback.h"

#include "MoqxRelay.h"

namespace openmoq::moqx {

void WeakRelayForwarderCallback::onEmpty(moxygen::MoQForwarder* forwarder) {
  onEmpty(forwarder->fullTrackName());
}

void WeakRelayForwarderCallback::forwardChanged(moxygen::MoQForwarder* forwarder, bool forward) {
  forwardChanged(forwarder->fullTrackName(), forward);
}

void WeakRelayForwarderCallback::newGroupRequested(
    moxygen::MoQForwarder* forwarder,
    uint64_t group
) {
  newGroupRequested(forwarder->fullTrackName(), group);
}

void WeakRelayForwarderCallback::onEmpty(const moxygen::FullTrackName& ftn) {
  if (auto r = relay_.lock()) {
    r->onEmptyImpl(ftn);
  }
}

void WeakRelayForwarderCallback::forwardChanged(const moxygen::FullTrackName& ftn, bool forward) {
  if (auto r = relay_.lock()) {
    r->forwardChangedImpl(ftn, forward);
  }
}

void WeakRelayForwarderCallback::newGroupRequested(
    const moxygen::FullTrackName& ftn,
    uint64_t group
) {
  if (auto r = relay_.lock()) {
    r->newGroupRequestedImpl(ftn, group);
  }
}

// TerminationFilter already drives MoqxRelay::onPublishDone; delegating here would double it.
void WeakRelayForwarderCallback::onPublishDone(const moxygen::FullTrackName& /*ftn*/) {}

} // namespace openmoq::moqx
