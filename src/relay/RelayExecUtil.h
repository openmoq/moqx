/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "MoQCrossExecFilter.h"
#include "MoQPublisherCrossExecFilter.h"
#include <folly/Executor.h>
#include <memory>
#include <utility>

namespace openmoq::moqx {

// Wraps c in a MoQCrossExecFilter targeting exec, or returns c if exec is null.
inline std::shared_ptr<moxygen::TrackConsumer>
maybeCrossExec(folly::Executor* exec, std::shared_ptr<moxygen::TrackConsumer> c) {
  if (!exec) {
    return c;
  }
  return std::make_shared<MoQCrossExecFilter>(exec, std::move(c));
}

// Wraps c in a MoQFetchCrossExecFilter targeting exec, or returns c if exec is null.
inline std::shared_ptr<moxygen::FetchConsumer>
maybeCrossExec(folly::Executor* exec, std::shared_ptr<moxygen::FetchConsumer> c) {
  if (!exec) {
    return c;
  }
  return std::make_shared<MoQFetchCrossExecFilter>(exec, std::move(c));
}

// Wraps p in a MoQPublisherCrossExecFilter targeting exec, or returns p if exec is null.
inline std::shared_ptr<moxygen::Publisher>
maybeCrossExec(folly::Executor* exec, std::shared_ptr<moxygen::Publisher> p) {
  if (!exec) {
    return p;
  }
  return std::make_shared<MoQPublisherCrossExecFilter>(exec, std::move(p));
}

// Runs fn on exec (fire-and-forget) if exec is non-null; otherwise runs it
// inline on the calling thread.
template <typename Fn>
void runOnExec(folly::Executor* exec, Fn&& fn) {
  if (exec) {
    exec->add(std::forward<Fn>(fn));
  } else {
    std::forward<Fn>(fn)();
  }
}

// Runs fn on relayExec (fire-and-forget) if non-null; otherwise inline.
// Convenience wrapper for dispatching to the relay executor.
template <typename Fn>
void runOnRelayExec(folly::Executor* relayExec, Fn&& fn) {
  runOnExec(relayExec, std::forward<Fn>(fn));
}

// When relayExec is set, dispatches fn to sessionExec (fire-and-forget).
// Otherwise runs fn inline (caller is already on the correct thread).
// Use this when relay state changes need to notify a specific session's executor.
template <typename Fn>
void runOnSessionExec(folly::Executor* relayExec, folly::Executor* sessionExec, Fn&& fn) {
  runOnExec(relayExec ? sessionExec : nullptr, std::forward<Fn>(fn));
}

} // namespace openmoq::moqx
