/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Originally from github.com/facebookexperimental/moxygen.
 * See the moxygen LICENSE for the original license terms:
 * https://github.com/openmoq/moxygen/blob/main/LICENSE
 *
 * Copyright (c) OpenMOQ contributors.
 */

#pragma once

#include "MoqxCache.h"
#include "NamespaceTree.h"
#include "PendingRendezvousTree.h"
#include "SubscriptionRegistry.h"
#include "UpstreamProvider.h"
#include "config/Config.h"
#include "relay/ForwarderRef.h"
#include "relay/PropertyRanking.h"
#include "relay/RelayExecUtil.h"
#include "stats/TrackStatsRegistry.h"
#include <moxygen/MoQSession.h>
#include <moxygen/events/MoQFollyExecutorImpl.h>
#include <moxygen/relay/MoQForwarder.h>
#include <moxygen/util/TimedBaton.h>

#include <folly/futures/ThreadWheelTimekeeper.h>

#include <folly/Executor.h>
#include <folly/Function.h>
#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openmoq::moqx {

// Draft 16 encodes Hop IDs as QUIC variable-length integers.
inline constexpr uint64_t kMaxRelayHopID = (uint64_t{1} << 62) - 1;

uint64_t generateRelayHopID();

class CrossExecFilter;

// Visitor interface for relay state inspection.
// MoqxRelay::dumpState() calls these methods while walking internal state.
// Implement this to serialize state into any format without adding format
// dependencies to MoqxRelay itself.
//
// Section callbacks bracket each group of items so visitors never need to
// track whether a section was empty or infer ordering from call patterns.
class RelayStateVisitor {
public:
  virtual ~RelayStateVisitor() = default;

  // --- Downstream peer section ---
  virtual void onPeersBegin() = 0;
  // Called for each connected downstream peer relay.
  virtual void onPeer(
      std::string_view address,
      std::string_view authority,
      std::string_view relayID // empty if peer didn't include one
  ) = 0;
  virtual void onPeersEnd() = 0;

  // --- Subscription section ---
  virtual void onSubscriptionsBegin() = 0;
  // Called for each track with an active subscription or publish.
  struct SubscriptionInfo {
    const moxygen::FullTrackName& ftn;
    bool isPublish;
    std::optional<moxygen::AbsoluteLocation> largest;
    uint64_t totalGroupsReceived{0};
    uint64_t totalObjectsReceived{0};
    std::string_view sourceAddress;
  };
  virtual void onSubscription(const SubscriptionInfo& info) = 0;
  virtual void onSubscriptionsEnd() = 0;

  // --- Namespace tree section ---
  virtual void onNamespaceTreeBegin() = 0;
  // Depth-first traversal. childKey is the map key in the parent's children
  // map; empty string for the root node.
  virtual void beginNamespaceNode(
      std::string_view childKey,
      const moxygen::TrackNamespace& ns,
      size_t sessionCount,
      std::string_view publisherAddress,
      std::string_view peerID
  ) = 0;
  virtual void endNamespaceNode() = 0;
  virtual void onNamespaceTreeEnd() = 0;

  // --- Cache section ---
  // Not called at all if the cache is disabled.
  virtual void onCacheBegin(size_t totalBytes, MoqxCache::TimePoint now) = 0;
  // Return false to stop the cache walk. The view does not outlive the call.
  virtual bool onCacheTrack(const MoqxCache::TrackStatsView& track) = 0;
  virtual void onCacheEnd() = 0;

  // False once the consumer is gone; the walk stops at the next section
  // boundary rather than formatting the rest for nobody.
  virtual bool alive() const = 0;
};

class MoqxRelay : public moxygen::Publisher,
                  public moxygen::Subscriber,
                  public std::enable_shared_from_this<MoqxRelay>,
                  public moxygen::MoQForwarder::Callback,
                  public NamespaceTree::Callback {
public:
  // Default for maxDeselected (tracks kept in deselected queue before eviction).
  // Set to 0 until pause/resume forwarding callbacks are wired in PropertyRanking;
  // a non-zero value without those callbacks is just topN+N with no benefit.
  static constexpr uint64_t kDefaultMaxDeselected = 0;
  static constexpr std::chrono::milliseconds kDefaultIdleTimeout{10'000};
  static constexpr std::chrono::milliseconds kDefaultActivityThreshold{2'000};

  // relayExec, when set, is owned by the relay and isolates all state on it;
  // null runs everything on the calling thread. useLocalForwarders (requires
  // relayExec) enables the per-thread local-forwarder data plane.
  static std::shared_ptr<MoqxRelay> create(
      config::CacheConfig cache = {},
      std::string relayID = {},
      uint64_t relayHopID = 0,
      std::shared_ptr<folly::Executor> relayExec = nullptr,
      bool useLocalForwarders = false,
      uint64_t maxDeselected = kDefaultMaxDeselected,
      std::chrono::milliseconds idleTimeout = kDefaultIdleTimeout,
      std::chrono::milliseconds activityThreshold = kDefaultActivityThreshold
  );

  folly::Executor* getRelayExec() const { return relayExec_; }
  uint64_t getRelayHopID() const { return relayHopID_; }

  // execs must cover every thread the data plane runs on (io threads plus
  // relayExec_).
  stats::TrackStatsRegistry& trackStatsRegistry() { return trackStats_; }
  const stats::TrackStatsRegistry& trackStatsRegistry() const { return trackStats_; }

  struct TrackMatch {
    std::vector<moxygen::FullTrackName> keys;
    // Total matches before the limit was applied.
    size_t matched{0};
  };

  // Must run on the relay exec.
  TrackMatch
  matchTracks(const moxygen::TrackNamespace& nsPrefix, const std::string* trackName, size_t limit)
      const;

  void setAllowedNamespacePrefix(moxygen::TrackNamespace allowed) {
    allowedNamespacePrefix_ = std::move(allowed);
  }

  // Returns the per-session publish/subscribe handler.
  virtual std::shared_ptr<moxygen::Publisher> createPublisherFilter() = 0;
  virtual std::shared_ptr<moxygen::Subscriber> createSubscriberFilter() = 0;

  // Store the upstream provider. The provider must have been constructed with
  // publishHandler=this and subscribeHandler=this so that the upstream relay's
  // reciprocal subNs and namespace announcements route through MoqxRelay.
  void setUpstreamProvider(std::shared_ptr<UpstreamProvider> upstream) {
    upstream_ = std::move(upstream);
  }

  // Force-evicts a specific track unconditionally. Not thread-safe.
  size_t purge(const moxygen::FullTrackName& ftn) { return cache_ ? cache_->purge(ftn) : 0; }

  // Force-evicts all tracks in the given namespace unconditionally. Not thread-safe.
  size_t purge(const moxygen::TrackNamespace& ns) { return cache_ ? cache_->purge(ns) : 0; }

  // Force-evicts all cached tracks unconditionally. Not thread-safe.
  size_t purge() { return cache_ ? cache_->purge() : 0; }

  // Stops and releases the upstream provider, breaking the shared_ptr cycle
  // between MoqxRelay and UpstreamProvider. Safe to call with no upstream.
  void stop() {
    if (upstream_) {
      upstream_->stop();
      // Do not reset upstream_ here: the provider's session/client live on the
      // worker EVB thread and must be freed there (via the reconnect coroutine's
      // shared_from_this dropping after it co_returns). Releasing upstream_ when
      // relay is destroyed naturally (services_ cleared) is safe because by then
      // stop() has already cleared the back-refs so relay's refcount == 1.
    }
  }

  // Called by UpstreamProvider's onConnect hook after a new upstream session is
  // established. Issues the peer subNs handshake and saves the handle.
  folly::coro::Task<void> onUpstreamConnect(std::shared_ptr<moxygen::MoQSession> session);

  // Called by UpstreamProvider's onDisconnect hook when the upstream session
  // closes. Releases the peer subNs handle.
  void onUpstreamDisconnect();

  // Releases per-session relay state; the servers call this as a session tears down.
  void onSessionEnd(std::shared_ptr<moxygen::MoQSession> session);

  folly::coro::Task<FetchResult>
  fetch(moxygen::Fetch fetch, std::shared_ptr<moxygen::FetchConsumer> consumer) override;

  folly::coro::Task<SubscribeNamespaceResult> subscribeNamespace(
      moxygen::SubscribeNamespace subNs,
      std::shared_ptr<NamespacePublishHandle> namespacePublishHandle
  ) override;

  folly::coro::Task<SubscribeTracksResult> subscribeTracks(
      moxygen::SubscribeTracks subTracks,
      std::shared_ptr<PublishBlockedHandle> publishBlockedHandle = nullptr
  ) override;

  folly::coro::Task<moxygen::Subscriber::PublishNamespaceResult>
  publishNamespace(moxygen::PublishNamespace pubNs, std::shared_ptr<moxygen::Subscriber::PublishNamespaceCallback>)
      override;

  void goaway(moxygen::Goaway goaway) override {
    XLOG(INFO) << "Processing goaway uri=" << goaway.newSessionUri;
  }

  folly::coro::Task<moxygen::Publisher::TrackStatusResult> trackStatus(moxygen::TrackStatus req
  ) override;

  std::shared_ptr<moxygen::MoQSession> findPublishNamespaceSession(const moxygen::TrackNamespace& ns
  ) {
    return namespaceTree_.findPublisherSession(ns);
  }

  std::vector<std::shared_ptr<moxygen::MoQSession>>
  findPublishNamespaceSessions(const moxygen::TrackNamespace& ns) {
    auto session = findPublishNamespaceSession(ns);
    if (session) {
      return {session};
    }
    return {};
  }

  // Sync cores of publishNamespace/publishNamespaceDone. Called by the
  // Subscriber coroutine interface and directly by MoqxRelayNamespaceHandle
  // (which provides the session explicitly — no getRequestSession() needed).
  std::shared_ptr<moxygen::Subscriber::PublishNamespaceHandle> doPublishNamespace(
      moxygen::PublishNamespace pubNs,
      std::shared_ptr<moxygen::MoQSession> session,
      std::shared_ptr<moxygen::Subscriber::PublishNamespaceCallback> callback,
      std::string peerID = {}
  );

  void doPublishNamespaceDone(
      const moxygen::TrackNamespace& trackNamespace,
      std::shared_ptr<moxygen::MoQSession> session
  );

  // Returns the upstream provider, or null if none is configured.
  std::shared_ptr<UpstreamProvider> upstreamProvider() const { return upstream_; }

  // Walks relay state by calling visitor methods. Synchronous throughout: it
  // iterates maps that a suspension would let another task mutate, so a visitor
  // must not block or suspend either.
  // Must run on the executor that owns relay state: getRelayExec(), or the
  // single io thread when there is none.
  void dumpState(RelayStateVisitor& visitor) const;

  // Test accessor: check if a publish exists and return node/publish state
  struct PublishState {
    bool nodeExists{false};                                // true if tree node exists
    std::shared_ptr<moxygen::MoQSession> session{nullptr}; // publish session if exists
  };
  PublishState findPublishState(const moxygen::FullTrackName& ftn);

  // Test accessor: pruning of a timed-out waiter's tree node happens
  // asynchronously, with no other externally observable signal that it
  // completed in time for a fast test to assert on.
  bool hasPendingRendezvousWaiters() const { return !pendingRendezvous_.empty(); }

protected:
  MoqxRelay(
      config::CacheConfig cache,
      std::string relayID,
      uint64_t relayHopID,
      std::shared_ptr<folly::Executor> relayExec,
      uint64_t maxDeselected,
      std::chrono::milliseconds idleTimeout,
      std::chrono::milliseconds activityThreshold
  );

  // === Execution-mode hooks ===

  // Releases the registry entry once its publisher has terminated.
  virtual void releasePublisherEntry(const moxygen::FullTrackName& ftn) = 0;

  // chainForwarder is null when another executor owns the forwarder.
  virtual void wireForwarderCallback(const std::shared_ptr<moxygen::MoQForwarder>& chainForwarder
  ) = 0;

  // Returns false on synchronous failure.
  virtual bool addSubscriberAndPublish(
      std::shared_ptr<moxygen::MoQSession> subscriberSession,
      const ForwarderRef& publisherRef,
      bool forward,
      bool pinned
  ) = 0;

  virtual ForwarderRef makeForwarderRef(
      const std::shared_ptr<moxygen::MoQForwarder>& forwarder,
      folly::Executor* publisherExec
  ) const = 0;

  // Build the filter chain for a track subscription: TopNFilter → RelayIngestFilter → (cache) →
  // forwarder. Used by both publish() and subscribe() paths to ensure consistent filter chain.
  virtual SubscriptionRegistry::FilterChainResult buildFilterChain(
      const moxygen::FullTrackName& ftn,
      std::shared_ptr<moxygen::MoQForwarder> forwarder
  ) = 0;

  // Resolves a joining fetch against the relay's forwarder, rewriting fetch to a
  // standalone one and clearing joining when it can.
  virtual std::optional<moxygen::FetchError> resolveJoiningFetchOnRelay(
      moxygen::Fetch& fetch,
      moxygen::JoiningFetch*& joining,
      const std::shared_ptr<moxygen::MoQSession>& session
  ) = 0;

  // Answers TRACK_STATUS from an active subscription's forwarder; nullopt goes upstream.
  virtual folly::coro::Task<std::optional<moxygen::TrackStatusOk>> readLocalTrackStatus(
      const SubscriptionRegistry::UpstreamView& upstreamView,
      const moxygen::TrackStatus& req
  ) = 0;

  // Runs evict against the forwarder that holds session's subscriber for ftn.
  virtual void evictOnOwner(
      const moxygen::FullTrackName& ftn,
      const std::shared_ptr<moxygen::MoQSession>& session,
      folly::Function<void(const std::shared_ptr<moxygen::MoQForwarder>&)> evict
  ) = 0;

  // The handle a downstream session calls unsubscribe() on for a relay-initiated PUBLISH.
  virtual std::shared_ptr<moxygen::Publisher::SubscriptionHandle>
  makePeerHandle(std::shared_ptr<moxygen::MoQForwarder::Subscriber> subscriber) {
    return subscriber;
  }

  struct IngestChain {
    std::shared_ptr<IngestCounters> ingest;
    std::shared_ptr<TopNFilter> topNFilter;
    std::shared_ptr<moxygen::TrackConsumer> chainHead;
  };
  // TopNFilter → RelayIngestFilter → downstream, wrapped in ingest track stats.
  IngestChain makeIngestChain(
      const moxygen::FullTrackName& ftn,
      std::shared_ptr<moxygen::TrackConsumer> downstream
  );

  class NamespaceSubscription;
  class TracksSubscription;
  class RelayIngestFilter;

  // No-op NamespaceTree::Callback for the tracks-subscriber tree.
  // The tracks tree never has publishers, so onPublishNamespaceDone never fires.
  struct NullCallback : public NamespaceTree::Callback {
    void onPublishNamespaceDone(const moxygen::TrackNamespace&) override {}
  };

  void unsubscribeNamespace(
      const moxygen::TrackNamespace& prefix,
      std::shared_ptr<moxygen::MoQSession> session
  );

  // Draft 18+
  void unsubscribeTracks(
      const moxygen::TrackNamespace& prefix,
      std::shared_ptr<moxygen::MoQSession> session
  );

  void onPublishDone(const moxygen::FullTrackName& ftn);

  void onPublishNamespaceDone(const moxygen::TrackNamespace& ns) override;

  NamespaceTree namespaceTree_{*this};

  // Draft 18+: parallel tree for SUBSCRIBE_TRACKS. Independent overlap space;
  // only `children` and `sessions` are populated (no publishers, no callbacks).
  NullCallback tracksTreeCb_;
  NamespaceTree tracksTree_{tracksTreeCb_};

  void onEmpty(moxygen::MoQForwarder* forwarder) override;
  void forwardChanged(moxygen::MoQForwarder* forwarder, bool forward) override;
  void newGroupRequested(moxygen::MoQForwarder* forwarder, uint64_t group) override;

  friend class WeakRelayForwarderCallback;

  // FTN-keyed impl variants — called by the MoQForwarder::Callback overrides
  // above (single-thread) or by WeakRelayForwarderCallback on relay exec.
  void onEmptyImpl(const moxygen::FullTrackName& ftn);
  void forwardChangedImpl(const moxygen::FullTrackName& ftn, bool forward);
  void newGroupRequestedImpl(const moxygen::FullTrackName& ftn, uint64_t group);

  folly::coro::Task<void> publishNamespaceToSession(
      std::shared_ptr<moxygen::MoQSession> session,
      moxygen::PublishNamespace pubNs,
      std::shared_ptr<NamespaceTree::NamespaceNode> nodePtr
  );

  struct PreparedPublish {
    std::shared_ptr<moxygen::MoQForwarder::Subscriber> subscriber;
    folly::coro::Task<folly::Expected<moxygen::PublishOk, moxygen::PublishError>> reply;
  };
  std::optional<PreparedPublish> startPublish(
      std::shared_ptr<moxygen::MoQSession> session,
      std::shared_ptr<moxygen::MoQForwarder> forwarder,
      bool forward,
      bool pinned,
      folly::Executor* subscriberExec
  );

  std::optional<moxygen::PublishError> validatePublishNamespace(
      const moxygen::FullTrackName& ftn,
      moxygen::RequestID requestID,
      bool emptyNamespaceAllowed
  ) const;

  static bool emptyNamespaceAllowed(const std::shared_ptr<moxygen::MoQSession>& session);

  // TRACK_FILTER support

  // Get or create PropertyRanking for the given property type on a namespace node.
  // Retroactively registers any tracks already published under that node.
  // ns must be the full namespace of `node` (used as BFS seed for track registration).
  std::shared_ptr<PropertyRanking> getOrCreateRanking(
      std::shared_ptr<NamespaceTree::NamespaceNode> node,
      uint64_t propertyType,
      const moxygen::TrackNamespace& ns
  );

  // Called by PropertyRanking when a track enters a session's top-N selection.
  void onTrackSelected(
      const moxygen::FullTrackName& ftn,
      std::shared_ptr<moxygen::MoQSession> session,
      bool forward
  );

  // Called by PropertyRanking when a track is evicted from a session's deselected queue.
  void
  onTrackEvicted(const moxygen::FullTrackName& ftn, std::shared_ptr<moxygen::MoQSession> session);

  moxygen::TrackNamespace allowedNamespacePrefix_;
  // Operational identity used by relay authentication and upstream routing.
  std::string relayID_;
  // Opaque random protocol identity required by draft-lcurley-moq-relay-hops.
  uint64_t relayHopID_;
  std::shared_ptr<UpstreamProvider> upstream_;

  // Holds the peer subNs handle for the upstream (initiating) direction.
  // Kept alive so the subscription is not cancelled when onUpstreamConnect returns.
  std::shared_ptr<moxygen::Publisher::SubscribeNamespaceHandle> upstreamSubNsHandle_;

  struct PeerInfo {
    std::shared_ptr<moxygen::Publisher::SubscribeNamespaceHandle> handle;
    std::string relayID; // from peer auth token; empty if not provided
  };
  // Reciprocal peer subNs handles: one per peer relay session that has
  // connected to us. Kept alive so the subscription is not immediately
  // cancelled. Keyed by raw session pointer (valid for session lifetime).
  folly::F14FastMap<moxygen::MoQSession*, PeerInfo> peerSubNsHandles_;

  struct LegacyPublisherHopID {
    std::weak_ptr<moxygen::MoQSession> session;
    uint64_t hopID;
  };
  folly::F14FastMap<const moxygen::MoQSession*, LegacyPublisherHopID> legacyPublisherHopIDs_;
  SubscriptionRegistry registry_;

  std::optional<std::vector<uint64_t>> ingestRelayHopPath(
      const moxygen::PublishNamespace& pubNs,
      const std::shared_ptr<moxygen::MoQSession>& session
  );

  uint64_t getOrCreateLegacyPublisherHopID(const std::shared_ptr<moxygen::MoQSession>& session);

  struct UpstreamOk {
    std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle;
    moxygen::RequestID requestID;
    moxygen::Extensions extensions;
    std::optional<moxygen::AbsoluteLocation> largest;
  };

  folly::coro::Task<folly::Expected<UpstreamOk, moxygen::SubscribeError>>
  subscribeUpstreamAndApplyOk(
      std::shared_ptr<moxygen::Publisher> upstream,
      moxygen::SubscribeRequest upstreamSubReq,
      std::shared_ptr<moxygen::TrackConsumer> upstreamConsumer,
      std::shared_ptr<moxygen::MoQForwarder> publisherFwd,
      moxygen::RequestID clientRequestID
  );

  std::optional<moxygen::SubscribeError> completeUpstreamSubscription(
      const moxygen::FullTrackName& ftn,
      UpstreamOk& upstreamOk,
      SubscriptionRegistry::UpstreamSubscribePending& pending,
      std::shared_ptr<moxygen::MoQSession> upstreamSession,
      std::shared_ptr<moxygen::Publisher> upstreamPublisher,
      moxygen::RequestID clientRequestID
  );

  // Impl methods — run on relayExec_ when set, or inline when relayExec_==nullptr.
  folly::coro::Task<FetchResult>
  fetchImpl(moxygen::Fetch fetch, std::shared_ptr<moxygen::FetchConsumer> consumer);
  folly::coro::Task<SubscribeNamespaceResult> subscribeNamespaceImpl(
      moxygen::SubscribeNamespace subNs,
      std::shared_ptr<NamespacePublishHandle> namespacePublishHandle
  );
  folly::coro::Task<moxygen::Subscriber::PublishNamespaceResult> publishNamespaceImpl(
      moxygen::PublishNamespace pubNs,
      std::shared_ptr<moxygen::Subscriber::PublishNamespaceCallback> callback
  );
  folly::coro::Task<moxygen::Publisher::TrackStatusResult> trackStatusImpl(moxygen::TrackStatus req
  );
  folly::coro::Task<void> onUpstreamConnectImpl(std::shared_ptr<moxygen::MoQSession> session);

  // Synchronous result of publishWithSession: the consumer the publisher writes
  // to and the PublishOk to return to the publisher.  Returned synchronously so
  // the reply coro (coPublish) can co_return the PublishOk immediately after
  // setup without waiting for any downstream peer handshake.
  struct PublishSetup {
    std::shared_ptr<moxygen::TrackConsumer> consumer; // Null in LF mode
    moxygen::PublishOk publishOk;
  };
  using PublishSetupResult = folly::Expected<PublishSetup, moxygen::PublishError>;

  // Contains all the inline publish() logic, taking session explicitly so it
  // can be called from either the I/O thread (relayExec_==nullptr) or from
  // coPublish on relay exec (where getRequestSession() would return null).
  PublishSetupResult publishWithSession(
      moxygen::PublishRequest pub,
      std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle,
      std::shared_ptr<moxygen::MoQSession> session,
      ForwarderRef publisherRef
  );

  std::shared_ptr<folly::Executor> ownedRelayExec_;
  std::unique_ptr<folly::EventBaseThreadTimekeeper> timekeeper_;
  folly::Executor* relayExec_{nullptr};

  std::shared_ptr<moxygen::Publisher> findUpstreamPublisher(const moxygen::TrackNamespace& ns) {
    auto session = namespaceTree_.findPublisherSession(ns);
    if (!session) {
      return nullptr;
    }
    return maybeWrapPublisher(relayExec_, std::move(session));
  }

  stats::TrackStatsRegistry trackStats_;

  std::unique_ptr<MoqxCache> cache_;
  uint64_t maxDeselected_{kDefaultMaxDeselected};

  // === Pending rendezvous (draft 18+ SUBSCRIBE with RENDEZVOUS_TIMEOUT) ===
  // Subscribers waiting on a namespace/track that isn't published yet. Woken by
  // doPublishNamespace()/publishWithSession() when matching content arrives;
  // otherwise each waiter's baton times out.
  PendingRendezvousTree pendingRendezvous_;

  // Existence check, then park. Callers screen first. Must run on relayExec_ (inline
  // in SingleThread mode): touches registry_/namespaceTree_/pendingRendezvous_.
  folly::coro::Task<std::optional<moxygen::SubscribeError>> rendezvousWithPublisherOrTimeout(
      const moxygen::SubscribeRequest& subReq,
      std::chrono::milliseconds timeout
  );

  std::chrono::milliseconds idleTimeout_{kDefaultIdleTimeout};
  std::chrono::milliseconds activityThreshold_{kDefaultActivityThreshold};
};

// Creates a NamespacePublishHandle that bridges NAMESPACE/NAMESPACE_DONE
// messages from a peer relay into relay->doPublishNamespace(). When relayExec
// is non-null, callbacks are dispatched to it so relay state is only mutated
// on the relay executor thread. Used for both the initiating (UpstreamProvider)
// and reciprocal (MoqxRelay) paths.
std::shared_ptr<moxygen::Publisher::NamespacePublishHandle> makeNamespaceBridgeHandle(
    std::weak_ptr<MoqxRelay> relay,
    std::shared_ptr<moxygen::MoQSession> session,
    std::string peerID = {},
    folly::Executor* relayExec = nullptr
);

} // namespace openmoq::moqx
