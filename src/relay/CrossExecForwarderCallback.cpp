/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "CrossExecForwarderCallback.h"

namespace openmoq::moqx {

void CrossExecForwarderCallback::onEmpty(moxygen::MoQForwarder* forwarder) {
  targetExec_->add([ftn = forwarder->fullTrackName(), downstream = downstream_]() mutable {
    downstream->onEmpty(ftn);
  });
}

void CrossExecForwarderCallback::onPublishDone(moxygen::MoQForwarder* forwarder) {
  targetExec_->add([ftn = forwarder->fullTrackName(), downstream = downstream_]() mutable {
    downstream->onPublishDone(ftn);
  });
}

void CrossExecForwarderCallback::forwardChanged(moxygen::MoQForwarder* forwarder, bool forward) {
  targetExec_->add([ftn = forwarder->fullTrackName(), downstream = downstream_, forward]() mutable {
    downstream->forwardChanged(ftn, forward);
  });
}

void CrossExecForwarderCallback::newGroupRequested(
    moxygen::MoQForwarder* forwarder,
    uint64_t group
) {
  targetExec_->add([ftn = forwarder->fullTrackName(), downstream = downstream_, group]() mutable {
    downstream->newGroupRequested(ftn, group);
  });
}

} // namespace openmoq::moqx
