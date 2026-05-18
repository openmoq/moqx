/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/MoQPublisherCrossExecFilter.h"

namespace openmoq::moqx {

folly::coro::Task<moxygen::Publisher::TrackStatusResult>
MoQPublisherCrossExecFilter::trackStatus(moxygen::TrackStatus trackStatus) {
  co_return co_await folly::coro::co_withExecutor(
      folly::getKeepAliveToken(targetExec_),
      inner_->trackStatus(std::move(trackStatus))
  );
}

folly::coro::Task<moxygen::Publisher::SubscribeResult> MoQPublisherCrossExecFilter::subscribe(
    moxygen::SubscribeRequest sub,
    std::shared_ptr<moxygen::TrackConsumer> callback
) {
  co_return co_await folly::coro::co_withExecutor(
      folly::getKeepAliveToken(targetExec_),
      inner_->subscribe(std::move(sub), std::move(callback))
  );
}

folly::coro::Task<moxygen::Publisher::FetchResult> MoQPublisherCrossExecFilter::fetch(
    moxygen::Fetch fetchReq,
    std::shared_ptr<moxygen::FetchConsumer> fetchCallback
) {
  co_return co_await folly::coro::co_withExecutor(
      folly::getKeepAliveToken(targetExec_),
      inner_->fetch(std::move(fetchReq), std::move(fetchCallback))
  );
}

folly::coro::Task<moxygen::Publisher::SubscribeNamespaceResult>
MoQPublisherCrossExecFilter::subscribeNamespace(
    moxygen::SubscribeNamespace subAnn,
    std::shared_ptr<NamespacePublishHandle> namespacePublishHandle
) {
  co_return co_await folly::coro::co_withExecutor(
      folly::getKeepAliveToken(targetExec_),
      inner_->subscribeNamespace(std::move(subAnn), std::move(namespacePublishHandle))
  );
}

void MoQPublisherCrossExecFilter::goaway(moxygen::Goaway goaway) {
  targetExec_->add([inner = inner_, goaway = std::move(goaway)]() mutable {
    inner->goaway(std::move(goaway));
  });
}

} // namespace openmoq::moqx
