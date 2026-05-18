/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "WeakRelayForwarderCallback.h"
namespace openmoq::moqx {

void WeakRelayForwarderCallback::onEmpty(moxygen::MoQForwarder* forwarder) {
  if (auto r = relay_.lock()) {
    r->onEmpty(forwarder);
  }
}

void WeakRelayForwarderCallback::forwardChanged(
    moxygen::MoQForwarder* forwarder,
    bool forward) {
  if (auto r = relay_.lock()) {
    r->forwardChanged(forwarder, forward);
  }
}

void WeakRelayForwarderCallback::newGroupRequested(
    moxygen::MoQForwarder* forwarder,
    uint64_t group) {
  if (auto r = relay_.lock()) {
    r->newGroupRequested(forwarder, group);
  }
}

} // namespace openmoq::moqx
