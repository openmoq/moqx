/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/events/MoQFollyExecutorImpl.h>

#include <folly/Executor.h>

namespace openmoq::moqx {

// True only while this thread runs exec's loop. False when exec is not EventBase-backed.
inline bool isOnExecutor(folly::Executor* exec) {
  auto* follyExec = dynamic_cast<moxygen::MoQFollyExecutorImpl*>(exec);
  return follyExec && follyExec->getBackingEventBase()->inRunningEventBaseThread();
}

// For debug asserts. Unlike isOnExecutor(), it also passes while exec's loop is not running, and
// for an exec that is not EventBase-backed.
inline bool onExecutor(folly::Executor* exec) {
  auto* follyExec = dynamic_cast<moxygen::MoQFollyExecutorImpl*>(exec);
  return !follyExec || follyExec->getBackingEventBase()->isInEventBaseThread();
}

} // namespace openmoq::moqx
