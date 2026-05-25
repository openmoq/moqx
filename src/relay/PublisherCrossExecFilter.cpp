/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/PublisherCrossExecFilter.h"

namespace openmoq::moqx {

folly::coro::Task<moxygen::Publisher::TrackStatusResult>
PublisherCrossExecFilter::trackStatus(moxygen::TrackStatus trackStatus) {
  co_return co_await folly::coro::co_withExecutor(
      folly::getKeepAliveToken(targetExec_),
      inner_->trackStatus(std::move(trackStatus))
  );
}

folly::coro::Task<moxygen::Publisher::SubscribeResult> PublisherCrossExecFilter::subscribe(
    moxygen::SubscribeRequest sub,
    std::shared_ptr<moxygen::TrackConsumer> callback
) {
  co_return co_await folly::coro::co_withExecutor(
      folly::getKeepAliveToken(targetExec_),
      inner_->subscribe(std::move(sub), std::move(callback))
  );
}

folly::coro::Task<moxygen::Publisher::FetchResult> PublisherCrossExecFilter::fetch(
    moxygen::Fetch fetchReq,
    std::shared_ptr<moxygen::FetchConsumer> fetchCallback
) {
  co_return co_await folly::coro::co_withExecutor(
      folly::getKeepAliveToken(targetExec_),
      inner_->fetch(std::move(fetchReq), std::move(fetchCallback))
  );
}

folly::coro::Task<moxygen::Publisher::SubscribeNamespaceResult>
PublisherCrossExecFilter::subscribeNamespace(
    moxygen::SubscribeNamespace subAnn,
    std::shared_ptr<NamespacePublishHandle> namespacePublishHandle
) {
  co_return co_await folly::coro::co_withExecutor(
      folly::getKeepAliveToken(targetExec_),
      inner_->subscribeNamespace(std::move(subAnn), std::move(namespacePublishHandle))
  );
}

void PublisherCrossExecFilter::goaway(moxygen::Goaway goaway) {
  targetExec_->add([inner = inner_, goaway = std::move(goaway)]() mutable {
    inner->goaway(std::move(goaway));
  });
}

} // namespace openmoq::moqx
