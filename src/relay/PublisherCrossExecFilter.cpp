/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/PublisherCrossExecFilter.h"
#include "relay/CrossExecFilter.h"
#include "relay/CrossExecSubscriptionHandle.h"
#include "relay/ExecBound.h"

namespace openmoq::moqx {

namespace {

// Readers take a handle's Ok when it is created, so each result wrapper here copies its inner's.
class CrossExecFetchHandle : public moxygen::Publisher::FetchHandle {
public:
  CrossExecFetchHandle(
      std::shared_ptr<moxygen::Publisher::FetchHandle> inner,
      folly::Executor* exec
  )
      : FetchHandle(inner->fetchOk()), inner_(std::move(inner), exec) {}

  void fetchCancel() override {
    inner_.add([](const auto& inner) { inner->fetchCancel(); });
  }

  folly::coro::Task<RequestUpdateResult> requestUpdate(moxygen::RequestUpdate update) override {
    return forwardRequestUpdate(inner_, std::move(update));
  }

private:
  ExecBound<std::shared_ptr<moxygen::Publisher::FetchHandle>> inner_;
};

// Wraps a NamespacePublishHandle so that namespaceMsg() and namespaceDoneMsg()
// are dispatched back to the caller's executor.  Held by the inner session on
// targetExec_ and called from there.
class CrossExecNamespacePublishHandle : public moxygen::Publisher::NamespacePublishHandle {
public:
  CrossExecNamespacePublishHandle(
      std::shared_ptr<moxygen::Publisher::NamespacePublishHandle> inner,
      folly::Executor* exec
  )
      : inner_(std::move(inner), exec) {}

  void namespaceMsg(const moxygen::TrackNamespace& ns) override {
    inner_.add([ns](const auto& inner) { inner->namespaceMsg(ns); });
  }

  void namespaceMsg(const moxygen::Namespace& ns) override {
    inner_.add([ns](const auto& inner) { inner->namespaceMsg(ns); });
  }

  void namespaceDoneMsg(const moxygen::TrackNamespace& ns) override {
    inner_.add([ns](const auto& inner) { inner->namespaceDoneMsg(ns); });
  }

private:
  ExecBound<std::shared_ptr<moxygen::Publisher::NamespacePublishHandle>> inner_;
};

class CrossExecSubscribeNamespaceHandle : public moxygen::Publisher::SubscribeNamespaceHandle {
public:
  CrossExecSubscribeNamespaceHandle(
      std::shared_ptr<moxygen::Publisher::SubscribeNamespaceHandle> inner,
      folly::Executor* exec
  )
      : SubscribeNamespaceHandle(inner->subscribeNamespaceOk()), inner_(std::move(inner), exec) {}

  void unsubscribeNamespace() override {
    inner_.add([](const auto& inner) { inner->unsubscribeNamespace(); });
  }

  folly::coro::Task<RequestUpdateResult> requestUpdate(moxygen::RequestUpdate update) override {
    return forwardRequestUpdate(inner_, std::move(update));
  }

private:
  ExecBound<std::shared_ptr<moxygen::Publisher::SubscribeNamespaceHandle>> inner_;
};

// Wraps a PublishBlockedHandle so publishBlocked() dispatches back to the
// caller's executor. Held by the inner session on targetExec_.
class CrossExecPublishBlockedHandle : public moxygen::Publisher::PublishBlockedHandle {
public:
  CrossExecPublishBlockedHandle(
      std::shared_ptr<moxygen::Publisher::PublishBlockedHandle> inner,
      folly::Executor* exec
  )
      : inner_(std::move(inner), exec) {}

  void publishBlocked(
      const moxygen::TrackNamespace& trackNamespaceSuffix,
      const std::string& trackName
  ) override {
    inner_.add([trackNamespaceSuffix, trackName](const auto& inner) {
      inner->publishBlocked(trackNamespaceSuffix, trackName);
    });
  }

private:
  ExecBound<std::shared_ptr<moxygen::Publisher::PublishBlockedHandle>> inner_;
};

class CrossExecSubscribeTracksHandle : public moxygen::Publisher::SubscribeTracksHandle {
public:
  CrossExecSubscribeTracksHandle(
      std::shared_ptr<moxygen::Publisher::SubscribeTracksHandle> inner,
      folly::Executor* exec
  )
      : SubscribeTracksHandle(inner->subscribeTracksOk()), inner_(std::move(inner), exec) {}

  void unsubscribeTracks() override {
    inner_.add([](const auto& inner) { inner->unsubscribeTracks(); });
  }

  folly::coro::Task<RequestUpdateResult> requestUpdate(moxygen::RequestUpdate update) override {
    return forwardRequestUpdate(inner_, std::move(update));
  }

private:
  ExecBound<std::shared_ptr<moxygen::Publisher::SubscribeTracksHandle>> inner_;
};

} // namespace

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
  auto callerExec = co_await folly::coro::co_current_executor;
  auto wrappedConsumer =
      CrossExecFilter::create(callerExec, std::move(callback), /*deepCopyPayload=*/false);
  auto* targetExec = targetExec_;
  auto result = co_await folly::coro::co_withExecutor(
      folly::getKeepAliveToken(targetExec),
      inner_->subscribe(std::move(sub), std::move(wrappedConsumer))
  );
  if (result.hasValue()) {
    auto handle =
        std::make_shared<CrossExecSubscriptionHandle>(std::move(result.value()), targetExec);
    co_await folly::coro::co_safe_point;
    co_return handle;
  }
  co_return result;
}

folly::coro::Task<moxygen::Publisher::FetchResult> PublisherCrossExecFilter::fetch(
    moxygen::Fetch fetchReq,
    std::shared_ptr<moxygen::FetchConsumer> fetchCallback
) {
  auto callerExec = co_await folly::coro::co_current_executor;
  auto wrappedConsumer =
      FetchCrossExecFilter::create(callerExec, std::move(fetchCallback), /*deepCopyPayload=*/false);
  auto consumerRef = wrappedConsumer;
  auto* targetExec = targetExec_;
  auto result = co_await folly::coro::co_withExecutor(
      folly::getKeepAliveToken(targetExec),
      inner_->fetch(std::move(fetchReq), std::move(wrappedConsumer))
  );
  if (result.hasValue()) {
    auto handle = std::make_shared<CrossExecFetchHandle>(std::move(result.value()), targetExec);
    co_await folly::coro::co_safe_point;
    co_return handle;
  }
  // inner never stored or used the consumer, so no lambdas are in-flight;
  // deactivate() releases selfGuard_ inline without dispatching.
  consumerRef->deactivate();
  co_return result;
}

folly::coro::Task<moxygen::Publisher::SubscribeNamespaceResult>
PublisherCrossExecFilter::subscribeNamespace(
    moxygen::SubscribeNamespace subNs,
    std::shared_ptr<NamespacePublishHandle> namespacePublishHandle
) {
  auto callerExec = co_await folly::coro::co_current_executor;
  auto wrappedHandle = namespacePublishHandle ? std::make_shared<CrossExecNamespacePublishHandle>(
                                                    std::move(namespacePublishHandle),
                                                    callerExec
                                                )
                                              : nullptr;
  auto* targetExec = targetExec_;
  auto result = co_await folly::coro::co_withExecutor(
      folly::getKeepAliveToken(targetExec),
      inner_->subscribeNamespace(std::move(subNs), std::move(wrappedHandle))
  );
  if (result.hasValue()) {
    auto handle =
        std::make_shared<CrossExecSubscribeNamespaceHandle>(std::move(result.value()), targetExec);
    co_await folly::coro::co_safe_point;
    co_return handle;
  }
  co_return result;
}

folly::coro::Task<moxygen::Publisher::SubscribeTracksResult>
PublisherCrossExecFilter::subscribeTracks(
    moxygen::SubscribeTracks subTracks,
    std::shared_ptr<PublishBlockedHandle> publishBlockedHandle
) {
  auto callerExec = co_await folly::coro::co_current_executor;
  auto wrappedHandle = publishBlockedHandle ? std::make_shared<CrossExecPublishBlockedHandle>(
                                                  std::move(publishBlockedHandle),
                                                  callerExec
                                              )
                                            : nullptr;
  auto* targetExec = targetExec_;
  auto result = co_await folly::coro::co_withExecutor(
      folly::getKeepAliveToken(targetExec),
      inner_->subscribeTracks(std::move(subTracks), std::move(wrappedHandle))
  );
  if (result.hasValue()) {
    auto handle =
        std::make_shared<CrossExecSubscribeTracksHandle>(std::move(result.value()), targetExec);
    co_await folly::coro::co_safe_point;
    co_return handle;
  }
  co_return result;
}

void PublisherCrossExecFilter::goaway(moxygen::Goaway goaway) {
  targetExec_->add([inner = inner_, goaway = std::move(goaway)]() mutable {
    inner->goaway(std::move(goaway));
  });
}

} // namespace openmoq::moqx
