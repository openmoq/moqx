/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MoQCrossExecForwarderCallback.h"

namespace openmoq::moqx {

void MoQCrossExecForwarderCallback::onEmpty(moxygen::MoQForwarder* /*forwarder*/) {
  targetExec_->add([forwarder = forwarder_, downstream = downstream_]() {
    if (auto f = forwarder.lock()) {
      downstream->onEmpty(f.get());
    }
  });
}

void MoQCrossExecForwarderCallback::forwardChanged(
    moxygen::MoQForwarder* /*forwarder*/,
    bool forward) {
  targetExec_->add([forwarder = forwarder_, downstream = downstream_, forward]() {
    if (auto f = forwarder.lock()) {
      downstream->forwardChanged(f.get(), forward);
    }
  });
}

void MoQCrossExecForwarderCallback::newGroupRequested(
    moxygen::MoQForwarder* /*forwarder*/,
    uint64_t group) {
  targetExec_->add([forwarder = forwarder_, downstream = downstream_, group]() {
    if (auto f = forwarder.lock()) {
      downstream->newGroupRequested(f.get(), group);
    }
  });
}

} // namespace openmoq::moqx
