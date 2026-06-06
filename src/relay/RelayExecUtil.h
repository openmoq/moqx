/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "CrossExecFilter.h"
#include "PublisherCrossExecFilter.h"
#include <folly/Executor.h>
#include <memory>
#include <moxygen/MoQSession.h>
#include <utility>

namespace openmoq::moqx {

// Wraps c in a CrossExecFilter targeting exec, or returns c if exec is null.
inline std::shared_ptr<moxygen::TrackConsumer>
maybeCrossExec(folly::Executor* exec, std::shared_ptr<moxygen::TrackConsumer> c) {
  if (!exec) {
    return c;
  }
  return std::make_shared<CrossExecFilter>(exec, std::move(c), /*deepCopyPayload=*/false);
}

// Wraps c in a FetchCrossExecFilter targeting exec, or returns c if exec is null.
inline std::shared_ptr<moxygen::FetchConsumer>
maybeCrossExec(folly::Executor* exec, std::shared_ptr<moxygen::FetchConsumer> c) {
  if (!exec) {
    return c;
  }
  return FetchCrossExecFilter::create(exec, std::move(c), /*deepCopyPayload=*/false);
}

// Wraps p in a PublisherCrossExecFilter targeting exec, or returns p if exec is null.
inline std::shared_ptr<moxygen::Publisher>
maybeCrossExec(folly::Executor* exec, std::shared_ptr<moxygen::Publisher> p) {
  if (!exec) {
    return p;
  }
  return std::make_shared<PublisherCrossExecFilter>(exec, std::move(p));
}

// Wraps session as a Publisher, targeting its executor when relayExec is set.
inline std::shared_ptr<moxygen::Publisher>
maybeWrapPublisher(folly::Executor* relayExec, std::shared_ptr<moxygen::MoQSession> session) {
  // Evaluate getExecutor() before std::move(session) to avoid unspecified
  // argument evaluation order leaving session moved-from.
  auto* exec = relayExec ? session->getExecutor() : nullptr;
  return maybeCrossExec(exec, std::shared_ptr<moxygen::Publisher>(std::move(session)));
}

// Runs fn on exec (fire-and-forget) if exec is non-null; otherwise runs it
// inline on the calling thread.
template <typename Fn> void runOnExec(folly::Executor* exec, Fn&& fn) {
  if (exec) {
    exec->add(std::forward<Fn>(fn));
  } else {
    std::forward<Fn>(fn)();
  }
}

// When relayExec is set, dispatches fn to sessionExec (fire-and-forget).
// Otherwise runs fn inline (caller is already on the correct thread).
// Use this when relay state changes need to notify a specific session's executor.
template <typename Fn>
void runOnSessionExec(folly::Executor* relayExec, folly::Executor* sessionExec, Fn&& fn) {
  runOnExec(relayExec ? sessionExec : nullptr, std::forward<Fn>(fn));
}

} // namespace openmoq::moqx
