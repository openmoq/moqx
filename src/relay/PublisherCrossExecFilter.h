/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/Executor.h>
#include <memory>
#include <moxygen/Publisher.h>

namespace openmoq::moqx {

// Forwards all Publisher calls to a target executor.
// Coroutine methods switch to targetExec_ before invoking the inner and return
// the result to the caller. goaway() is fire-and-forget.
//
// Requires targetExec_ to be a FIFO executor if call ordering matters.
class PublisherCrossExecFilter : public moxygen::Publisher {
public:
  PublisherCrossExecFilter(folly::Executor* targetExec, std::shared_ptr<moxygen::Publisher> inner)
      : targetExec_(targetExec), inner_(std::move(inner)) {}

  folly::coro::Task<TrackStatusResult> trackStatus(moxygen::TrackStatus trackStatus) override;

  folly::coro::Task<SubscribeResult> subscribe(
      moxygen::SubscribeRequest sub,
      std::shared_ptr<moxygen::TrackConsumer> callback
  ) override;

  folly::coro::Task<FetchResult>
  fetch(moxygen::Fetch fetchReq, std::shared_ptr<moxygen::FetchConsumer> fetchCallback) override;

  folly::coro::Task<SubscribeNamespaceResult> subscribeNamespace(
      moxygen::SubscribeNamespace subAnn,
      std::shared_ptr<NamespacePublishHandle> namespacePublishHandle
  ) override;

  folly::coro::Task<SubscribeTracksResult> subscribeTracks(
      moxygen::SubscribeTracks subTracks,
      std::shared_ptr<PublishBlockedHandle> publishBlockedHandle = nullptr
  ) override;

  void goaway(moxygen::Goaway goaway) override;

private:
  folly::Executor* targetExec_;
  std::shared_ptr<moxygen::Publisher> inner_;
};

} // namespace openmoq::moqx
