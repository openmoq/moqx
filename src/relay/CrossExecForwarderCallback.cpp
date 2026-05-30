/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "CrossExecForwarderCallback.h"

namespace openmoq::moqx {

void CrossExecForwarderCallback::onEmpty(moxygen::MoQForwarder* /*forwarder*/) {
  auto f = forwarder_.lock();
  if (!f) {
    return;
  }
  targetExec_->add([f = std::move(f), downstream = downstream_]() mutable {
    downstream->onEmpty(f.get());
  });
}

void CrossExecForwarderCallback::forwardChanged(
    moxygen::MoQForwarder* /*forwarder*/,
    bool forward
) {
  auto f = forwarder_.lock();
  if (!f) {
    return;
  }
  targetExec_->add([f = std::move(f), downstream = downstream_, forward]() mutable {
    downstream->forwardChanged(f.get(), forward);
  });
}

void CrossExecForwarderCallback::newGroupRequested(
    moxygen::MoQForwarder* /*forwarder*/,
    uint64_t group
) {
  auto f = forwarder_.lock();
  if (!f) {
    return;
  }
  targetExec_->add([f = std::move(f), downstream = downstream_, group]() mutable {
    downstream->newGroupRequested(f.get(), group);
  });
}

} // namespace openmoq::moqx
