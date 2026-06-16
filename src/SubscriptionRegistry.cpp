/*
 * Copyright (c) OpenMOQ contributors.
 */

#include "SubscriptionRegistry.h"

#include <folly/logging/xlog.h>

namespace openmoq::moqx {

// === UpstreamSubscribePending ===

bool SubscriptionRegistry::UpstreamSubscribePending::complete(
    std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle,
    moxygen::RequestID requestID,
    std::shared_ptr<moxygen::MoQSession> upstreamSession,
    std::shared_ptr<moxygen::Publisher> publisher
) {
  active_ = false;
  return registry_->completeSubscription(
      ftn_,
      weakForwarder_,
      std::move(handle),
      requestID,
      std::move(upstreamSession),
      std::move(publisher)
  );
}

SubscriptionRegistry::UpstreamSubscribePending::~UpstreamSubscribePending() {
  if (active_) {
    registry_->failAndRemove(ftn_, weakForwarder_);
  }
}

// === SubscriptionRegistry ===

std::variant<
    SubscriptionRegistry::FirstSubscriber,
    folly::coro::Task<SubscriptionRegistry::SubsequentSubscriber>>
SubscriptionRegistry::getOrCreateFromSubscribe(
    const moxygen::FullTrackName& ftn,
    std::shared_ptr<moxygen::MoQForwarder::Callback> callback,
    folly::FunctionRef<FilterChainResult(std::shared_ptr<moxygen::MoQForwarder>)> chainBuilder,
    std::optional<moxygen::AbsoluteLocation> largest
) {
  auto it = subscriptions_.find(ftn);
  if (it == subscriptions_.end()) {
    auto forwarder = std::make_shared<moxygen::MoQForwarder>(ftn, largest);
    forwarder->setCallback(std::move(callback));
    auto [consumer, topNFilter] = chainBuilder(forwarder);
    auto [emplaceIt, inserted] = subscriptions_.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(ftn),
        std::forward_as_tuple(forwarder, nullptr)
    );
    emplaceIt->second.topNFilter = topNFilter;
    return FirstSubscriber{
        forwarder,
        std::move(consumer),
        UpstreamSubscribePending{this, ftn, forwarder}
    };
  }

  // Subsequent subscriber: await in a proper coroutine function (not a lambda)
  // so the parameters live in the heap-allocated coroutine frame, not on the
  // stack of getOrCreateFromSubscribe.
  return awaitSubsequent(this, ftn, it->second.promise.getFuture());
}

folly::coro::Task<SubscriptionRegistry::SubsequentSubscriber> SubscriptionRegistry::awaitSubsequent(
    SubscriptionRegistry* registry,
    moxygen::FullTrackName ftn,
    folly::coro::Future<folly::Unit> future
) {
  co_await std::move(future);                    // throws on first-subscriber failure
  auto sit = registry->subscriptions_.find(ftn); // MUST re-find after suspension
  if (sit == registry->subscriptions_.end()) {
    XLOG(ERR) << "Subscription is GONE for " << ftn;
    co_yield folly::coro::co_error(std::runtime_error("subscription gone"));
  }
  co_return SubsequentSubscriber{sit->second.forwarder};
}

bool SubscriptionRegistry::completeSubscription(
    const moxygen::FullTrackName& ftn,
    std::weak_ptr<moxygen::MoQForwarder> weakForwarder,
    std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle,
    moxygen::RequestID requestID,
    std::shared_ptr<moxygen::MoQSession> upstreamSession,
    std::shared_ptr<moxygen::Publisher> publisher
) {
  auto it = subscriptions_.find(ftn);
  if (it == subscriptions_.end() || it->second.forwarder != weakForwarder.lock()) {
    return false;
  }
  auto& rsub = it->second;
  rsub.handle = std::move(handle);
  rsub.requestID = requestID;
  rsub.upstream = std::move(upstreamSession);
  rsub.publisher = std::move(publisher);
  rsub.promise.setValue(folly::unit);
  return true;
}

void SubscriptionRegistry::failAndRemove(
    const moxygen::FullTrackName& ftn,
    std::weak_ptr<moxygen::MoQForwarder> weakForwarder
) {
  auto it = subscriptions_.find(ftn);
  if (it != subscriptions_.end() && it->second.forwarder == weakForwarder.lock()) {
    it->second.promise.setException(std::runtime_error("upstream subscribe failed"));
    subscriptions_.erase(it);
  }
}

SubscriptionRegistry::PublishEntry SubscriptionRegistry::createFromPublish(
    const moxygen::FullTrackName& ftn,
    std::shared_ptr<moxygen::MoQForwarder> forwarder,
    std::shared_ptr<moxygen::MoQSession> session,
    std::shared_ptr<moxygen::Publisher> publisher,
    moxygen::RequestID requestID,
    std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle,
    folly::FunctionRef<FilterChainResult(std::shared_ptr<moxygen::MoQForwarder>)> chainBuilder
) {
  std::optional<Evicted> evicted;

  // Move out forwarder+handle before erasing to avoid use-after-free:
  // publishDone → onEmpty may fire during the erase.
  auto it = subscriptions_.find(ftn);
  if (it != subscriptions_.end()) {
    evicted = Evicted{std::move(it->second.forwarder), std::move(it->second.handle)};
    subscriptions_.erase(it);
  }

  auto [emplaceIt, inserted] = subscriptions_.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(ftn),
      std::forward_as_tuple(forwarder, session)
  );
  auto& rsub = emplaceIt->second;
  rsub.promise.setValue(folly::unit);
  rsub.requestID = requestID;
  rsub.handle = std::move(handle);
  rsub.publisher = std::move(publisher);
  rsub.isPublish = true;

  auto [consumer, topNFilter] = chainBuilder(forwarder);
  rsub.topNFilter = topNFilter;
  topNFilter->setActivityTarget(&rsub.lastObjectTime);

  return PublishEntry{std::move(consumer), std::move(evicted)};
}

bool SubscriptionRegistry::exists(const moxygen::FullTrackName& ftn) const {
  return subscriptions_.find(ftn) != subscriptions_.end();
}

std::shared_ptr<moxygen::MoQForwarder>
SubscriptionRegistry::getForwarder(const moxygen::FullTrackName& ftn) const {
  auto it = subscriptions_.find(ftn);
  return it != subscriptions_.end() ? it->second.forwarder : nullptr;
}

std::optional<SubscriptionRegistry::TopNView>
SubscriptionRegistry::getTopNView(const moxygen::FullTrackName& ftn) const {
  auto it = subscriptions_.find(ftn);
  if (it == subscriptions_.end()) {
    return std::nullopt;
  }
  return TopNView{it->second.forwarder, it->second.topNFilter, it->second.lastObjectTime};
}

std::optional<SubscriptionRegistry::UpstreamView>
SubscriptionRegistry::getUpstreamView(const moxygen::FullTrackName& ftn) const {
  auto it = subscriptions_.find(ftn);
  if (it == subscriptions_.end()) {
    return std::nullopt;
  }
  const auto& rsub = it->second;
  return UpstreamView{
      rsub.forwarder,
      rsub.publisher,
      rsub.handle,
      rsub.requestID,
      rsub.isPublish,
      rsub.promise.isFulfilled()
  };
}

std::optional<SubscriptionRegistry::FetchView>
SubscriptionRegistry::getFetchView(const moxygen::FullTrackName& ftn) const {
  auto it = subscriptions_.find(ftn);
  if (it == subscriptions_.end()) {
    return std::nullopt;
  }
  const auto& rsub = it->second;
  return FetchView{rsub.forwarder, rsub.publisher, rsub.requestID, rsub.promise.isFulfilled()};
}

std::shared_ptr<moxygen::MoQForwarder>
SubscriptionRegistry::onPublisherTerminated(const moxygen::FullTrackName& ftn) {
  auto it = subscriptions_.find(ftn);
  if (it == subscriptions_.end()) {
    return nullptr;
  }
  auto& rsub = it->second;
  rsub.handle.reset();
  rsub.upstream.reset();
  rsub.publisher.reset();
  if (rsub.forwarder->empty()) {
    subscriptions_.erase(it);
    return nullptr;
  }
  return rsub.forwarder;
}

void SubscriptionRegistry::remove(const moxygen::FullTrackName& ftn) {
  subscriptions_.erase(ftn);
}

void SubscriptionRegistry::removeIf(
    folly::FunctionRef<bool(const moxygen::FullTrackName&, const EntryView&)> predicate
) {
  for (auto it = subscriptions_.begin(); it != subscriptions_.end();) {
    EntryView view{
        it->first,
        it->second.forwarder,
        it->second.upstream,
        it->second.isPublish,
        it->second.lastObjectTime
    };
    if (predicate(it->first, view)) {
      it = subscriptions_.erase(it);
    } else {
      ++it;
    }
  }
}

void SubscriptionRegistry::forEach(folly::FunctionRef<void(const EntryView&)> fn) const {
  for (const auto& [ftn, rsub] : subscriptions_) {
    fn(EntryView{ftn, rsub.forwarder, rsub.upstream, rsub.isPublish, rsub.lastObjectTime});
  }
}

} // namespace openmoq::moqx
