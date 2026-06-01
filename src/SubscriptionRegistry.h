/*
 * Copyright (c) OpenMOQ contributors.
 */

#pragma once

#include "relay/TopNFilter.h"
#include <folly/Function.h>
#include <folly/container/F14Map.h>
#include <folly/coro/SharedPromise.h>
#include <folly/coro/Task.h>
#include <moxygen/MoQSession.h>
#include <moxygen/relay/MoQForwarder.h>

#include <chrono>
#include <memory>
#include <optional>
#include <variant>

namespace openmoq::moqx {

class SubscriptionRegistry {
public:
  struct FilterChainResult {
    std::shared_ptr<moxygen::TrackConsumer> consumer;
    std::shared_ptr<TopNFilter> topNFilter;
  };

  // === Subscribe path ===

  // Represents the window between emplacing a new subscription entry and the
  // upstream subscribe completing. complete() closes it on success; the destructor
  // closes it on failure (sets exception on promise, erases entry).
  // Does NOT undo addSubscriber() — only cleans up the registry entry.
  class UpstreamSubscribePending {
  public:
    UpstreamSubscribePending(
        SubscriptionRegistry* registry,
        moxygen::FullTrackName ftn,
        std::weak_ptr<moxygen::MoQForwarder> weakForwarder
    )
        : registry_(registry), ftn_(std::move(ftn)), weakForwarder_(std::move(weakForwarder)) {}

    UpstreamSubscribePending(UpstreamSubscribePending&& o) noexcept
        : registry_(o.registry_), ftn_(std::move(o.ftn_)),
          weakForwarder_(std::move(o.weakForwarder_)), active_(o.active_) {
      o.active_ = false;
    }

    UpstreamSubscribePending& operator=(UpstreamSubscribePending&&) = delete;
    UpstreamSubscribePending(const UpstreamSubscribePending&) = delete;
    UpstreamSubscribePending& operator=(const UpstreamSubscribePending&) = delete;

    // Identity-checked success path. Re-finds by ftn; checks forwarder identity
    // to detect a reconnecting publisher that replaced the entry during the
    // caller's co_await suspension. Sets handle, requestID, upstreamSession,
    // publisher; fulfills promise. Returns false if entry is gone or replaced.
    bool complete(
        std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle,
        moxygen::RequestID requestID,
        std::shared_ptr<moxygen::MoQSession> upstreamSession,
        std::shared_ptr<moxygen::Publisher> publisher
    );

    ~UpstreamSubscribePending();

  private:
    SubscriptionRegistry* registry_;
    moxygen::FullTrackName ftn_;
    std::weak_ptr<moxygen::MoQForwarder> weakForwarder_;
    bool active_{true};
  };

  struct FirstSubscriber {
    std::shared_ptr<moxygen::MoQForwarder> forwarder;
    std::shared_ptr<moxygen::TrackConsumer> consumer;
    UpstreamSubscribePending pending;
  };

  struct SubsequentSubscriber {
    std::shared_ptr<moxygen::MoQForwarder> forwarder;
  };

  // Synchronous. First path: creates entry, calls chainBuilder, returns
  // FirstSubscriber immediately. Subsequent path: returns a Task that
  // co_awaits the promise then re-finds (throws on first-subscriber failure).
  std::variant<FirstSubscriber, folly::coro::Task<SubsequentSubscriber>> getOrCreateFromSubscribe(
      const moxygen::FullTrackName& ftn,
      std::shared_ptr<moxygen::MoQForwarder::Callback> callback,
      folly::FunctionRef<FilterChainResult(std::shared_ptr<moxygen::MoQForwarder>)> chainBuilder,
      std::optional<moxygen::AbsoluteLocation> largest = std::nullopt
  );

  // === Publish path ===

  struct Evicted {
    std::shared_ptr<moxygen::MoQForwarder> forwarder;
    std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle; // may be null
  };

  struct PublishEntry {
    std::shared_ptr<moxygen::TrackConsumer> consumer;
    std::optional<Evicted> evicted;
  };

  // Creates entry, pre-fulfills promise, wires activity tracking.
  // Evicts any prior entry before emplacing — caller must call
  // publishDone/unsubscribe on evicted data.
  PublishEntry createFromPublish(
      const moxygen::FullTrackName& ftn,
      std::shared_ptr<moxygen::MoQForwarder> forwarder,
      std::shared_ptr<moxygen::MoQSession> session,
      std::shared_ptr<moxygen::Publisher> publisher,
      moxygen::RequestID requestID,
      std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle,
      folly::FunctionRef<FilterChainResult(std::shared_ptr<moxygen::MoQForwarder>)> chainBuilder
  );

  // === Lookup ===

  bool exists(const moxygen::FullTrackName& ftn) const;
  std::shared_ptr<moxygen::MoQForwarder> getForwarder(const moxygen::FullTrackName& ftn) const;

  struct TopNView {
    std::shared_ptr<moxygen::MoQForwarder> forwarder;
    std::shared_ptr<TopNFilter> topNFilter; // may be null for subscribe-path tracks
    std::chrono::steady_clock::time_point lastObjectTime;
  };
  std::optional<TopNView> getTopNView(const moxygen::FullTrackName& ftn) const;

  // For onEmpty / forwardChanged / newGroupRequested / trackStatus
  struct UpstreamView {
    std::shared_ptr<moxygen::MoQForwarder> forwarder;
    std::shared_ptr<moxygen::Publisher> publisher;
    std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle;
    moxygen::RequestID requestID;
    folly::Executor* publisherExec{nullptr}; // executor the primary forwarder lives on
    bool isPublish;
    bool isReady; // promise fulfilled
  };
  std::optional<UpstreamView> getUpstreamView(const moxygen::FullTrackName& ftn) const;

  // For fetch()
  struct FetchView {
    std::shared_ptr<moxygen::MoQForwarder> forwarder;
    std::shared_ptr<moxygen::Publisher> publisher;
    moxygen::RequestID requestID;
    bool isReady;
  };
  std::optional<FetchView> getFetchView(const moxygen::FullTrackName& ftn) const;

  // === Lifecycle transitions ===

  // Clears handle + upstream. Erases if no subscribers remain (returns null).
  // If subscribers remain, entry persists; caller must call remove() from onEmpty.
  std::shared_ptr<moxygen::MoQForwarder> onPublisherTerminated(const moxygen::FullTrackName& ftn);

  // Called from onEmpty after handle->unsubscribe() (subscribe-mode), or after
  // a publisher-terminated entry's forwarder goes empty.
  void remove(const moxygen::FullTrackName& ftn);

  // === Iteration ===

  struct EntryView {
    const moxygen::FullTrackName& ftn;
    std::shared_ptr<moxygen::MoQForwarder> forwarder;
    std::shared_ptr<moxygen::MoQSession> upstream;
    bool isPublish;
    std::chrono::steady_clock::time_point lastObjectTime;
  };

  void removeIf(folly::FunctionRef<bool(const moxygen::FullTrackName&, const EntryView&)> predicate
  );

  void forEach(folly::FunctionRef<void(const EntryView&)> fn) const;

private:
  struct RelaySubscription {
    RelaySubscription(
        std::shared_ptr<moxygen::MoQForwarder> f,
        std::shared_ptr<moxygen::MoQSession> u
    )
        : forwarder(std::move(f)), upstream(std::move(u)),
          lastObjectTime(std::chrono::steady_clock::now()) {}

    std::shared_ptr<moxygen::MoQForwarder> forwarder;
    std::shared_ptr<moxygen::MoQSession> upstream;
    std::shared_ptr<moxygen::Publisher> publisher;
    moxygen::RequestID requestID{0};
    std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle;
    folly::coro::SharedPromise<folly::Unit> promise;
    bool isPublish{false};
    std::shared_ptr<TopNFilter> topNFilter;
    std::chrono::steady_clock::time_point lastObjectTime;
  };

  // Called by UpstreamSubscribePending::complete().
  bool completeSubscription(
      const moxygen::FullTrackName& ftn,
      std::weak_ptr<moxygen::MoQForwarder> weakForwarder,
      std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle,
      moxygen::RequestID requestID,
      std::shared_ptr<moxygen::MoQSession> upstreamSession,
      std::shared_ptr<moxygen::Publisher> publisher
  );

  // Called by UpstreamSubscribePending destructor on failure.
  void failAndRemove(
      const moxygen::FullTrackName& ftn,
      std::weak_ptr<moxygen::MoQForwarder> weakForwarder
  );

  // Standalone coroutine for the subsequent-subscriber path. Parameters are
  // passed by value so they live in the heap-allocated coroutine frame rather
  // than in a lambda closure on the caller's stack.
  static folly::coro::Task<SubsequentSubscriber> awaitSubsequent(
      SubscriptionRegistry* registry,
      moxygen::FullTrackName ftn,
      folly::coro::Future<folly::Unit> future
  );

  folly::F14NodeMap<moxygen::FullTrackName, RelaySubscription, moxygen::FullTrackName::hash>
      subscriptions_;
};

} // namespace openmoq::moqx
