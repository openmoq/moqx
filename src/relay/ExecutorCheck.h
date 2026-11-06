/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/events/MoQFollyExecutorImpl.h>

#include <folly/Executor.h>

namespace openmoq::moqx {

// For debug asserts. Also true when exec is not EventBase-backed, since its thread is unknown.
inline bool onExecutor(folly::Executor* exec) {
  auto* follyExec = dynamic_cast<moxygen::MoQFollyExecutorImpl*>(exec);
  return !follyExec || follyExec->getBackingEventBase()->isInEventBaseThread();
}

} // namespace openmoq::moqx
