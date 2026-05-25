/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/SubscriberCrossExecFilter.h"

#include "relay/CrossExecFilter.h"

namespace openmoq::moqx {

folly::coro::Task<moxygen::Subscriber::PublishNamespaceResult>
SubscriberCrossExecFilter::publishNamespace(
    moxygen::PublishNamespace ann,
    std::shared_ptr<PublishNamespaceCallback> callback
) {
  co_return co_await folly::coro::co_withExecutor(
      folly::getKeepAliveToken(targetExec_),
      inner_->publishNamespace(std::move(ann), std::move(callback))
  );
}

moxygen::Subscriber::PublishResult SubscriberCrossExecFilter::publish(
    moxygen::PublishRequest pub,
    std::shared_ptr<moxygen::SubscriptionHandle> handle
) {
  auto filter = std::make_shared<CrossExecFilter>(targetExec_, nullptr);
  auto reply = coPublish(std::move(pub), std::move(handle), filter);
  return PublishConsumerAndReplyTask{std::move(filter), std::move(reply)};
}

folly::coro::Task<folly::Expected<moxygen::PublishOk, moxygen::PublishError>>
SubscriberCrossExecFilter::coPublish(
    moxygen::PublishRequest pub,
    std::shared_ptr<moxygen::SubscriptionHandle> handle,
    std::shared_ptr<CrossExecFilter> filter
) {
  co_return co_await folly::coro::co_withExecutor(
      folly::getKeepAliveToken(targetExec_),
      [inner = inner_,
       pub = std::move(pub),
       handle = std::move(handle),
       filter = std::move(filter)](
      ) mutable -> folly::coro::Task<folly::Expected<moxygen::PublishOk, moxygen::PublishError>> {
        auto result = inner->publish(std::move(pub), std::move(handle));
        if (result.hasError()) {
          return folly::coro::makeTask<folly::Expected<moxygen::PublishOk, moxygen::PublishError>>(
              folly::makeUnexpected(result.error())
          );
        }
        filter->setDownstream(std::move(result.value().consumer));
        return std::move(result.value().reply);
      }()
  );
}

void SubscriberCrossExecFilter::goaway(moxygen::Goaway goaway) {
  targetExec_->add([inner = inner_, goaway = std::move(goaway)]() mutable {
    inner->goaway(std::move(goaway));
  });
}

} // namespace openmoq::moqx
