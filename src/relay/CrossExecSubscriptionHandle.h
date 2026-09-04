/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

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
  )
      : inner_(std::move(inner)), exec_(exec) {
    // hasSubscribeOk() is non-virtual and reads the base's own field, so it
    // answers for the wrapper, not for inner_. Copy it or callers stop seeing one.
    if (inner_->hasSubscribeOk()) {
      setSubscribeOk(inner_->subscribeOk());
    }
  }

  ~CrossExecSubscriptionHandle() override {
    // Inner dtor may touch owner state; destroy it on exec_, not the dropping thread.
    if (inner_) {
      exec_->add([inner = std::move(inner_)]() mutable {});
    }
  }

  const moxygen::SubscribeOk& subscribeOk() const override { return inner_->subscribeOk(); }

  void unsubscribe() override {
    exec_->add([inner = inner_]() mutable { inner->unsubscribe(); });
  }

  folly::coro::Task<RequestUpdateResult> requestUpdate(moxygen::RequestUpdate update) override {
    co_return co_await folly::coro::co_withExecutor(
        folly::getKeepAliveToken(exec_),
        inner_->requestUpdate(std::move(update))
    );
  }

private:
  std::shared_ptr<moxygen::SubscriptionHandle> inner_;
  folly::Executor* exec_;
};

} // namespace openmoq::moqx
