/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "relay/ExecBound.h"

#include <folly/Executor.h>
#include <folly/coro/Task.h>
#include <memory>
#include <moxygen/MoQConsumers.h>
#include <moxygen/Publisher.h>

namespace openmoq::moqx {

// Dispatches unsubscribe() and requestUpdate() to the executor that owns the wrapped
// handle: a session handle held by the relay, or a forwarder Subscriber held by a session.
class CrossExecSubscriptionHandle : public moxygen::SubscriptionHandle {
public:
  CrossExecSubscriptionHandle(
      std::shared_ptr<moxygen::SubscriptionHandle> inner,
      folly::Executor* exec
  ) {
    // hasSubscribeOk() is non-virtual and reads the base's own field, so it
    // answers for the wrapper, not for inner_. Copy it or callers stop seeing one.
    // Readers take the Ok when the handle is created, so a copy is enough.
    if (inner->hasSubscribeOk()) {
      setSubscribeOk(inner->subscribeOk());
    }
    inner_ = {std::move(inner), exec};
  }

  void unsubscribe() override {
    inner_.post([](const auto& inner) { inner->unsubscribe(); });
  }

  folly::coro::Task<RequestUpdateResult> requestUpdate(moxygen::RequestUpdate update) override {
    co_return co_await inner_.run([update = std::move(update)](const auto& inner) mutable {
      return inner->requestUpdate(std::move(update));
    });
  }

private:
  ExecBound<std::shared_ptr<moxygen::SubscriptionHandle>> inner_;
};

} // namespace openmoq::moqx
