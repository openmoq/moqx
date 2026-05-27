/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/SubscriberCrossExecFilter.h"

#include "relay/CrossExecFilter.h"

namespace openmoq::moqx {

namespace {

// Wraps a PublishNamespaceCallback so that publishNamespaceCancel() is
// dispatched back to the caller's executor.  Held by the inner session on
// targetExec_ and called from there.
class CrossExecPublishNamespaceCallback : public moxygen::Subscriber::PublishNamespaceCallback {
public:
  CrossExecPublishNamespaceCallback(
      std::shared_ptr<moxygen::Subscriber::PublishNamespaceCallback> inner,
      folly::Executor::KeepAlive<> exec
  )
      : inner_(std::move(inner)), exec_(std::move(exec)) {}

  void publishNamespaceCancel(
      moxygen::PublishNamespaceErrorCode errorCode,
      std::string reasonPhrase
  ) override {
    exec_->add([inner = inner_, errorCode, reason = std::move(reasonPhrase)]() mutable {
      inner->publishNamespaceCancel(errorCode, std::move(reason));
    });
  }

private:
  std::shared_ptr<moxygen::Subscriber::PublishNamespaceCallback> inner_;
  folly::Executor::KeepAlive<> exec_;
};

// Wraps a PublishNamespaceHandle so that publishNamespaceDone() and
// requestUpdate() are dispatched to targetExec_ (the inner session's exec).
// Held by the caller and invoked from the caller's executor.
class CrossExecPublishNamespaceHandle : public moxygen::Subscriber::PublishNamespaceHandle {
public:
  CrossExecPublishNamespaceHandle(
      std::shared_ptr<moxygen::Subscriber::PublishNamespaceHandle> inner,
      folly::Executor* exec
  )
      : inner_(std::move(inner)), exec_(exec) {}

  const moxygen::PublishNamespaceOk& publishNamespaceOk() const override {
    return inner_->publishNamespaceOk();
  }

  void publishNamespaceDone() override {
    exec_->add([inner = inner_]() mutable { inner->publishNamespaceDone(); });
  }

  folly::coro::Task<RequestUpdateResult> requestUpdate(moxygen::RequestUpdate update) override {
    co_return co_await folly::coro::co_withExecutor(
        folly::getKeepAliveToken(exec_),
        inner_->requestUpdate(std::move(update))
    );
  }

private:
  std::shared_ptr<moxygen::Subscriber::PublishNamespaceHandle> inner_;
  folly::Executor* exec_;
};

} // namespace

folly::coro::Task<moxygen::Subscriber::PublishNamespaceResult>
SubscriberCrossExecFilter::publishNamespace(
    moxygen::PublishNamespace pubNs,
    std::shared_ptr<PublishNamespaceCallback> callback
) {
  auto callerExec = co_await folly::coro::co_current_executor;
  auto wrappedCallback = callback ? std::make_shared<CrossExecPublishNamespaceCallback>(
                                        std::move(callback),
                                        std::move(callerExec)
                                    )
                                  : nullptr;
  auto result = co_await folly::coro::co_withExecutor(
      folly::getKeepAliveToken(targetExec_),
      inner_->publishNamespace(std::move(pubNs), std::move(wrappedCallback))
  );
  if (result.hasValue()) {
    co_return std::make_shared<CrossExecPublishNamespaceHandle>(
        std::move(result.value()),
        targetExec_
    );
  }
  co_return result;
}

moxygen::Subscriber::PublishResult SubscriberCrossExecFilter::publish(
    moxygen::PublishRequest pub,
    std::shared_ptr<moxygen::SubscriptionHandle> handle
) {
  auto filter = std::make_shared<CrossExecFilter>(targetExec_, nullptr);
  // co_invoke captures exec and inner by value here (while `this` is live),
  // so the task's coroutine frame owns them and doesn't need `this` to survive.
  auto reply = folly::coro::co_invoke(
      [exec = targetExec_,
       inner = inner_,
       pub = std::move(pub),
       handle = std::move(handle),
       filter](
      ) mutable -> folly::coro::Task<folly::Expected<moxygen::PublishOk, moxygen::PublishError>> {
        co_return co_await folly::coro::co_withExecutor(
            folly::getKeepAliveToken(exec),
            doPublishOnExec(std::move(inner), std::move(pub), std::move(handle), std::move(filter))
        );
      }
  );
  return PublishConsumerAndReplyTask{std::move(filter), std::move(reply)};
}

folly::coro::Task<folly::Expected<moxygen::PublishOk, moxygen::PublishError>>
SubscriberCrossExecFilter::doPublishOnExec(
    std::shared_ptr<moxygen::Subscriber> inner,
    moxygen::PublishRequest pub,
    std::shared_ptr<moxygen::SubscriptionHandle> handle,
    std::shared_ptr<CrossExecFilter> filter
) {
  auto result = inner->publish(std::move(pub), std::move(handle));
  if (result.hasError()) {
    co_return folly::makeUnexpected(result.error());
  }
  filter->setDownstream(std::move(result.value().consumer));
  co_return co_await std::move(result.value().reply);
}

void SubscriberCrossExecFilter::goaway(moxygen::Goaway goaway) {
  targetExec_->add([inner = inner_, goaway = std::move(goaway)]() mutable {
    inner->goaway(std::move(goaway));
  });
}

} // namespace openmoq::moqx
