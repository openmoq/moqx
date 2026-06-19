/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/PublisherCrossExecFilter.h"
#include "relay/CrossExecFilter.h"

namespace openmoq::moqx {

namespace {

// Dispatches unsubscribe() and requestUpdate() to the subscriber session's
// executor (targetExec), which owns the session state.  Held by the relay
// on relay exec and called back from there.
class CrossExecSubscriptionHandle : public moxygen::SubscriptionHandle {
public:
  CrossExecSubscriptionHandle(
      std::shared_ptr<moxygen::SubscriptionHandle> inner,
      folly::Executor* exec
  )
      : inner_(std::move(inner)), exec_(exec) {}

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

class CrossExecFetchHandle : public moxygen::Publisher::FetchHandle {
public:
  CrossExecFetchHandle(
      std::shared_ptr<moxygen::Publisher::FetchHandle> inner,
      folly::Executor* exec
  )
      : inner_(std::move(inner)), exec_(exec) {}

  const moxygen::FetchOk& fetchOk() const override { return inner_->fetchOk(); }

  void fetchCancel() override {
    exec_->add([inner = inner_]() mutable { inner->fetchCancel(); });
  }

  folly::coro::Task<RequestUpdateResult> requestUpdate(moxygen::RequestUpdate update) override {
    co_return co_await folly::coro::co_withExecutor(
        folly::getKeepAliveToken(exec_),
        inner_->requestUpdate(std::move(update))
    );
  }

private:
  std::shared_ptr<moxygen::Publisher::FetchHandle> inner_;
  folly::Executor* exec_;
};

// Wraps a NamespacePublishHandle so that namespaceMsg() and namespaceDoneMsg()
// are dispatched back to the caller's executor.  Held by the inner session on
// targetExec_ and called from there.
class CrossExecNamespacePublishHandle : public moxygen::Publisher::NamespacePublishHandle {
public:
  CrossExecNamespacePublishHandle(
      std::shared_ptr<moxygen::Publisher::NamespacePublishHandle> inner,
      folly::Executor::KeepAlive<> exec
  )
      : inner_(std::move(inner)), exec_(std::move(exec)) {}

  void namespaceMsg(const moxygen::TrackNamespace& ns) override {
    exec_->add([inner = inner_, ns]() mutable { inner->namespaceMsg(ns); });
  }

  void namespaceDoneMsg(const moxygen::TrackNamespace& ns) override {
    exec_->add([inner = inner_, ns]() mutable { inner->namespaceDoneMsg(ns); });
  }

private:
  std::shared_ptr<moxygen::Publisher::NamespacePublishHandle> inner_;
  folly::Executor::KeepAlive<> exec_;
};

class CrossExecSubscribeNamespaceHandle : public moxygen::Publisher::SubscribeNamespaceHandle {
public:
  CrossExecSubscribeNamespaceHandle(
      std::shared_ptr<moxygen::Publisher::SubscribeNamespaceHandle> inner,
      folly::Executor* exec
  )
      : inner_(std::move(inner)), exec_(exec) {}

  const moxygen::SubscribeNamespaceOk& subscribeNamespaceOk() const override {
    return inner_->subscribeNamespaceOk();
  }

  void unsubscribeNamespace() override {
    exec_->add([inner = inner_]() mutable { inner->unsubscribeNamespace(); });
  }

  folly::coro::Task<RequestUpdateResult> requestUpdate(moxygen::RequestUpdate update) override {
    co_return co_await folly::coro::co_withExecutor(
        folly::getKeepAliveToken(exec_),
        inner_->requestUpdate(std::move(update))
    );
  }

private:
  std::shared_ptr<moxygen::Publisher::SubscribeNamespaceHandle> inner_;
  folly::Executor* exec_;
};

// Wraps a PublishBlockedHandle so publishBlocked() dispatches back to the
// caller's executor. Held by the inner session on targetExec_.
class CrossExecPublishBlockedHandle : public moxygen::Publisher::PublishBlockedHandle {
public:
  CrossExecPublishBlockedHandle(
      std::shared_ptr<moxygen::Publisher::PublishBlockedHandle> inner,
      folly::Executor::KeepAlive<> exec
  )
      : inner_(std::move(inner)), exec_(std::move(exec)) {}

  void publishBlocked(
      const moxygen::TrackNamespace& trackNamespaceSuffix,
      const std::string& trackName
  ) override {
    exec_->add([inner = inner_, trackNamespaceSuffix, trackName]() mutable {
      inner->publishBlocked(trackNamespaceSuffix, trackName);
    });
  }

private:
  std::shared_ptr<moxygen::Publisher::PublishBlockedHandle> inner_;
  folly::Executor::KeepAlive<> exec_;
};

class CrossExecSubscribeTracksHandle : public moxygen::Publisher::SubscribeTracksHandle {
public:
  CrossExecSubscribeTracksHandle(
      std::shared_ptr<moxygen::Publisher::SubscribeTracksHandle> inner,
      folly::Executor* exec
  )
      : inner_(std::move(inner)), exec_(exec) {}

  const moxygen::SubscribeTracksOk& subscribeTracksOk() const override {
    return inner_->subscribeTracksOk();
  }

  void unsubscribeTracks() override {
    exec_->add([inner = inner_]() mutable { inner->unsubscribeTracks(); });
  }

  folly::coro::Task<RequestUpdateResult> requestUpdate(moxygen::RequestUpdate update) override {
    co_return co_await folly::coro::co_withExecutor(
        folly::getKeepAliveToken(exec_),
        inner_->requestUpdate(std::move(update))
    );
  }

private:
  std::shared_ptr<moxygen::Publisher::SubscribeTracksHandle> inner_;
  folly::Executor* exec_;
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
      std::make_shared<CrossExecFilter>(callerExec, std::move(callback), /*deepCopyPayload=*/false);
  auto result = co_await folly::coro::co_withExecutor(
      folly::getKeepAliveToken(targetExec_),
      inner_->subscribe(std::move(sub), std::move(wrappedConsumer))
  );
  if (result.hasValue()) {
    co_return std::make_shared<CrossExecSubscriptionHandle>(std::move(result.value()), targetExec_);
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
  auto result = co_await folly::coro::co_withExecutor(
      folly::getKeepAliveToken(targetExec_),
      inner_->fetch(std::move(fetchReq), std::move(wrappedConsumer))
  );
  if (result.hasValue()) {
    co_return std::make_shared<CrossExecFetchHandle>(std::move(result.value()), targetExec_);
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
                                                    std::move(callerExec)
                                                )
                                              : nullptr;
  auto result = co_await folly::coro::co_withExecutor(
      folly::getKeepAliveToken(targetExec_),
      inner_->subscribeNamespace(std::move(subNs), std::move(wrappedHandle))
  );
  if (result.hasValue()) {
    co_return std::make_shared<CrossExecSubscribeNamespaceHandle>(
        std::move(result.value()),
        targetExec_
    );
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
                                                  std::move(callerExec)
                                              )
                                            : nullptr;
  auto result = co_await folly::coro::co_withExecutor(
      folly::getKeepAliveToken(targetExec_),
      inner_->subscribeTracks(std::move(subTracks), std::move(wrappedHandle))
  );
  if (result.hasValue()) {
    co_return std::make_shared<CrossExecSubscribeTracksHandle>(
        std::move(result.value()),
        targetExec_
    );
  }
  co_return result;
}

void PublisherCrossExecFilter::goaway(moxygen::Goaway goaway) {
  targetExec_->add([inner = inner_, goaway = std::move(goaway)]() mutable {
    inner->goaway(std::move(goaway));
  });
}

} // namespace openmoq::moqx
