/*
 * Copyright (c) OpenMOQ contributors.
 */

#pragma once

#include "relay/ForwarderRef.h"
#include "relay/RecentGroupWindow.h"
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

// Ingest counters for one track, maintained on the relay executor by
// MoqxRelay::RelayIngestFilter. /state reads these instead of the forwarder's
// own: in LocalForwarder mode the forwarder lives on another executor.
//
// shared_ptr, not a pointer into the entry: the filter chain can outlive its
// registry entry while a publisher drains.
struct IngestCounters {
  std::optional<moxygen::AbsoluteLocation> largest;
  uint64_t groups{0};
  uint64_t objects{0};

  // Once per incoming object, whatever the delivery mode.
  void record(uint64_t group, uint64_t object) {
    ++objects;
    if (recentGroups_.admit(group)) {
      ++groups;
    }
    moxygen::AbsoluteLocation loc{group, object};
    if (!largest || *largest < loc) {
      largest = loc;
    }
  }

private:
  RecentGroupWindow recentGroups_;
};

class SubscriptionRegistry {
public:
  // Names one entry for as long as it sits in the map, so a caller that suspends
  // can tell its own entry from one a reconnecting publisher put in its place.
  enum class EntryEpoch : uint64_t {};

  struct FilterChainResult {
    std::shared_ptr<moxygen::TrackConsumer> consumer;
    std::shared_ptr<TopNFilter> topNFilter;
    // Differs from consumer in LocalForwarder mode, where the relay chain
    // hangs off a channel subscriber rather than the publisher's writes.
    std::shared_ptr<moxygen::TrackConsumer> chainHead;
    std::shared_ptr<IngestCounters> ingest;
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
        EntryEpoch epoch
    )
        : registry_(registry), ftn_(std::move(ftn)), epoch_(epoch) {}

    UpstreamSubscribePending(UpstreamSubscribePending&& o) noexcept
        : registry_(o.registry_), ftn_(std::move(o.ftn_)), epoch_(o.epoch_), active_(o.active_) {
      o.active_ = false;
    }

    UpstreamSubscribePending& operator=(UpstreamSubscribePending&&) = delete;
    UpstreamSubscribePending(const UpstreamSubscribePending&) = delete;
    UpstreamSubscribePending& operator=(const UpstreamSubscribePending&) = delete;

    // Re-finds by ftn and checks the epoch, so a reconnecting publisher that
    // replaced the entry during the caller's co_await suspension is not mistaken
    // for it. Sets handle, requestID, upstreamSession, publisher; fulfills
    // promise. Returns false if entry is gone or replaced.
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
    EntryEpoch epoch_;
    bool active_{true};
  };

  struct FirstSubscriber {
    // The only strong handle the caller gets; the entry itself holds a ForwarderRef.
    std::shared_ptr<moxygen::MoQForwarder> forwarder;
    std::shared_ptr<moxygen::TrackConsumer> consumer;
    UpstreamSubscribePending pending;
  };

  struct SubsequentSubscriber {
    ForwarderRef forwarder;
  };

  // refMaker found no publisher to build a ref against; the map is untouched.
  struct NoPublisher {};

  // Synchronous. First path: creates entry, calls chainBuilder, returns
  // FirstSubscriber immediately. Subsequent path: returns a Task that
  // co_awaits the promise then re-finds (throws on first-subscriber failure).
  // refMaker runs only on the first path and owns the decision to proceed.
  std::variant<FirstSubscriber, folly::coro::Task<SubsequentSubscriber>, NoPublisher>
  getOrCreateFromSubscribe(
      const moxygen::FullTrackName& ftn,
      std::shared_ptr<moxygen::MoQForwarder::Callback> callback,
      folly::FunctionRef<FilterChainResult(std::shared_ptr<moxygen::MoQForwarder>)> chainBuilder,
      folly::FunctionRef<std::optional<ForwarderRef>(const std::shared_ptr<moxygen::MoQForwarder>&)>
          refMaker,
      std::optional<moxygen::AbsoluteLocation> largest = std::nullopt
  );

  // === Publish path ===

  struct Evicted {
    ForwarderRef forwarder;
    std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle; // may be null
    folly::Executor* publisherExec{nullptr}; // old publisher's session exec, for handle teardown
  };

  struct PublishEntry {
    std::shared_ptr<moxygen::TrackConsumer> consumer;
    std::optional<Evicted> evicted;
  };

  // Creates entry, pre-fulfills promise, wires activity tracking. `forwarder` is what the entry
  // stores (remote in LF mode; owned otherwise). Evicts any prior entry before emplacing — caller
  // must call publishDone/unsubscribe on evicted data.
  PublishEntry createFromPublish(
      const moxygen::FullTrackName& ftn,
      ForwarderRef forwarder,
      std::shared_ptr<moxygen::MoQSession> session,
      std::shared_ptr<moxygen::Publisher> publisher,
      moxygen::RequestID requestID,
      std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle,
      folly::FunctionRef<FilterChainResult()> chainBuilder
  );

  // === Lookup ===

  bool exists(const moxygen::FullTrackName& ftn) const;
  // Empty ref if there is no entry.
  ForwarderRef getForwarderRef(const moxygen::FullTrackName& ftn) const;

  struct TopNView {
    ForwarderRef forwarder;
    std::shared_ptr<TopNFilter> topNFilter; // may be null for subscribe-path tracks
    std::shared_ptr<moxygen::TrackConsumer> chainHead;
    std::chrono::steady_clock::time_point lastObjectTime;
  };
  std::optional<TopNView> getTopNView(const moxygen::FullTrackName& ftn) const;

  // For onEmpty / forwardChanged / newGroupRequested / trackStatus
  struct UpstreamView {
    ForwarderRef forwarder;
    std::shared_ptr<moxygen::Publisher> publisher;
    std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle;
    moxygen::RequestID requestID;
    folly::Executor* publisherExec{nullptr}; // executor the publisher forwarder lives on
    bool isPublish;
    bool isReady; // promise fulfilled
  };
  std::optional<UpstreamView> getUpstreamView(const moxygen::FullTrackName& ftn) const;

  // For fetch()
  struct FetchView {
    ForwarderRef forwarder;
    std::shared_ptr<moxygen::Publisher> publisher;
    moxygen::RequestID requestID;
    bool isReady;
  };
  std::optional<FetchView> getFetchView(const moxygen::FullTrackName& ftn) const;

  // === Lifecycle transitions ===

  // LF mode: replaces the entry's ref once ownership is anchored elsewhere (the
  // publisher exec's LocalForwarderRegistry), so the relay exec holds no strong ref.

  // Clears handle + upstream. Erases if no subscribers remain (returns empty).
  // If subscribers remain, entry persists; caller must call remove() from onEmpty.
  ForwarderRef onPublisherTerminated(const moxygen::FullTrackName& ftn);

  // Called from onEmpty after handle->unsubscribe() (subscribe-mode), or after
  // a publisher-terminated entry's forwarder goes empty.
  void remove(const moxygen::FullTrackName& ftn);

  // === Iteration ===

  struct EntryView {
    const moxygen::FullTrackName& ftn;
    const ForwarderRef& forwarder;
    std::shared_ptr<moxygen::MoQSession> upstream;
    bool isPublish;
    std::chrono::steady_clock::time_point lastObjectTime;
    const IngestCounters& ingest;
  };

  void removeIf(folly::FunctionRef<bool(const EntryView&)> predicate);

  void forEach(folly::FunctionRef<void(const EntryView&)> fn) const;

  void forEachName(folly::FunctionRef<void(const moxygen::FullTrackName&)> fn) const;

private:
  struct RelaySubscription {
    RelaySubscription(ForwarderRef f, std::shared_ptr<moxygen::MoQSession> u, EntryEpoch e)
        : forwarder(std::move(f)), epoch(e), upstream(std::move(u)),
          lastObjectTime(std::chrono::steady_clock::now()) {}

    ForwarderRef forwarder;
    EntryEpoch epoch;
    std::shared_ptr<moxygen::MoQSession> upstream;
    std::shared_ptr<moxygen::Publisher> publisher;
    moxygen::RequestID requestID{0};
    std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle;
    folly::coro::SharedPromise<folly::Unit> promise;
    bool isPublish{false};
    std::shared_ptr<TopNFilter> topNFilter;
    std::shared_ptr<moxygen::TrackConsumer> chainHead;
    std::chrono::steady_clock::time_point lastObjectTime;
    std::shared_ptr<IngestCounters> ingest;
  };

  // Called by UpstreamSubscribePending::complete().
  bool completeSubscription(
      const moxygen::FullTrackName& ftn,
      EntryEpoch epoch,
      std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle,
      moxygen::RequestID requestID,
      std::shared_ptr<moxygen::MoQSession> upstreamSession,
      std::shared_ptr<moxygen::Publisher> publisher
  );

  // Called by UpstreamSubscribePending destructor on failure.
  void failAndRemove(const moxygen::FullTrackName& ftn, EntryEpoch epoch);

  // Standalone coroutine for the subsequent-subscriber path. Parameters are
  // passed by value so they live in the heap-allocated coroutine frame rather
  // than in a lambda closure on the caller's stack.
  static folly::coro::Task<SubsequentSubscriber> awaitSubsequent(
      SubscriptionRegistry* registry,
      moxygen::FullTrackName ftn,
      folly::coro::Future<folly::Unit> future
  );

  EntryEpoch nextEpoch() { return static_cast<EntryEpoch>(++epochCounter_); }

  folly::F14NodeMap<moxygen::FullTrackName, RelaySubscription, moxygen::FullTrackName::hash>
      subscriptions_;
  // Confined to the relay executor, so a plain counter.
  uint64_t epochCounter_{0};
};

} // namespace openmoq::moqx
