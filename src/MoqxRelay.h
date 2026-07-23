/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Originally from github.com/facebookexperimental/moxygen.
 * See deps/moxygen/LICENSE for the original license terms.
 *
 * Copyright (c) OpenMOQ contributors.
 */

#pragma once

#include "MoqxCache.h"
#include "NamespaceTree.h"
#include "SubscriptionRegistry.h"
#include "UpstreamProvider.h"
#include "config/Config.h"
#include "relay/LocalForwarderRegistry.h"
#include "relay/PropertyRanking.h"
#include "relay/RelayExecUtil.h"
#include <moxygen/MoQSession.h>
#include <moxygen/relay/MoQForwarder.h>

#include <folly/Executor.h>
#include <folly/ThreadLocal.h>
#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openmoq::moqx {

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
    size_t subscribers;
    uint64_t forwardingSubscribers;
    std::optional<moxygen::AbsoluteLocation> largest;
    uint64_t totalGroupsReceived;
    uint64_t totalObjectsReceived;
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
  // Called once with cache state; not called if cache is disabled.
  virtual void onCacheStats(
      size_t totalBytes,
      const std::vector<MoqxCache::TrackStats>& tracks,
      MoqxCache::TimePoint now
  ) = 0;
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
  explicit MoqxRelay(
      config::CacheConfig cache = {},
      std::string relayID = {},
      std::shared_ptr<folly::Executor> relayExec = nullptr,
      bool useLocalForwarders = false,
      uint64_t maxDeselected = kDefaultMaxDeselected,
      std::chrono::milliseconds idleTimeout = kDefaultIdleTimeout,
      std::chrono::milliseconds activityThreshold = kDefaultActivityThreshold,
      uint64_t relayHopID = 0
  )
      : relayID_(std::move(relayID)),
        relayHopID_(relayHopID == 0 ? moxygen::generateRelayHopID() : relayHopID),
        ownedRelayExec_(std::move(relayExec)), relayExec_(ownedRelayExec_.get()),
        useLocalForwarders_(useLocalForwarders), maxDeselected_(maxDeselected),
        idleTimeout_(idleTimeout), activityThreshold_(activityThreshold) {
    XCHECK_LE(relayHopID_, moxygen::kMaxRelayHopID);
    if (cache.maxCachedTracks > 0) {
      cache_ = std::make_unique<MoqxCache>(cache.maxCachedTracks, cache.maxCachedGroupsPerTrack);
      cache_->setMaxCachedBytes(static_cast<size_t>(cache.maxCachedMb) * 1024 * 1024);
      cache_->setMinEvictionBytes(static_cast<size_t>(cache.minEvictionKb) * 1024);
      cache_->setDefaultMaxCacheDuration(cache.defaultMaxCacheDuration);
      cache_->setMaxAllowedCacheDuration(cache.maxCacheDuration);
    }
  }

  folly::Executor* getRelayExec() const { return relayExec_; }
  uint64_t getRelayHopID() const { return relayHopID_; }

  void setAllowedNamespacePrefix(moxygen::TrackNamespace allowed) {
    allowedNamespacePrefix_ = std::move(allowed);
  }

  // Returns the per-session publish/subscribe handler: a local-forwarder or
  // cross-exec filter when relayExec_ is set, otherwise the relay itself.
  std::shared_ptr<moxygen::Publisher> createPublisherFilter();
  std::shared_ptr<moxygen::Subscriber> createSubscriberFilter();

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

  folly::coro::Task<SubscribeResult> subscribe(
      moxygen::SubscribeRequest subReq,
      std::shared_ptr<moxygen::TrackConsumer> consumer
  ) override;

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

  PublishResult publish(
      moxygen::PublishRequest pubReq,
      std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle = nullptr
  ) override;

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

  // Walks relay state by calling visitor methods.
  // MUST be called on the relay's worker EVB.
  void dumpState(RelayStateVisitor& visitor) const;

  // Test accessor: check if a publish exists and return node/publish state
  struct PublishState {
    bool nodeExists{false};                                // true if tree node exists
    std::shared_ptr<moxygen::MoQSession> session{nullptr}; // publish session if exists
  };
  PublishState findPublishState(const moxygen::FullTrackName& ftn);

private:
  class NamespaceSubscription;
  class TracksSubscription;
  class TerminationFilter;
  class LocalSubscribeFilter;
  class LocalPublishFilter;

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

  struct LocalForwarderBootstrap {
    std::shared_ptr<moxygen::MoQForwarder> localFwd;
    bool isNew{false};
    LocalForwarderRegistry* localReg{nullptr};
  };
  LocalForwarderBootstrap acquireLocalForwarder(
      const moxygen::FullTrackName& ftn,
      folly::FunctionRef<std::shared_ptr<moxygen::MoQForwarder>()> factory
  );

  bool addSubscriberAndPublish(
      std::shared_ptr<moxygen::MoQSession> subscriberSession,
      std::shared_ptr<moxygen::MoQForwarder> forwarder,
      bool forward,
      bool pinned,
      folly::Executor* publisherExec = nullptr
  );

  folly::coro::Task<void> addSubscriberAndPublishViaLocalForwarder(
      std::shared_ptr<moxygen::MoQSession> subscriberSession,
      std::shared_ptr<moxygen::MoQForwarder> publisherFwd,
      folly::Executor* publisherExec,
      bool forward,
      bool pinned
  );

  moxygen::Subscriber::PublishResult publishFromPublisherExec(
      moxygen::PublishRequest pub,
      std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle,
      std::shared_ptr<moxygen::MoQSession> session
  );

  // Constructs the publisher's local forwarder and installs its callback chain on
  // publisherExec. tlForwarders_ must already be initialized.
  std::shared_ptr<moxygen::MoQForwarder> createPublisherForwarder(const moxygen::PublishRequest& pub
  );

  std::optional<moxygen::PublishError>
  validatePublishNamespace(const moxygen::FullTrackName& ftn, moxygen::RequestID requestID) const;

  folly::coro::Task<folly::Expected<moxygen::PublishOk, moxygen::PublishError>>
  registerPublishOnRelayExec(
      moxygen::PublishRequest pub,
      std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle,
      std::shared_ptr<moxygen::MoQSession> session,
      std::shared_ptr<moxygen::MoQForwarder> publisherFwd,
      std::shared_ptr<CrossExecFilter> relayChainFilter
  );

  // TRACK_FILTER support

  // Build the filter chain for a track subscription: TopNFilter → TerminationFilter → (cache) →
  // forwarder. Used by both publish() and subscribe() paths to ensure consistent filter chain.
  SubscriptionRegistry::FilterChainResult buildFilterChain(
      const moxygen::FullTrackName& ftn,
      std::shared_ptr<moxygen::MoQForwarder> forwarder
  );

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
  std::string relayID_;
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
  SubscriptionRegistry registry_;

  std::shared_ptr<moxygen::TrackConsumer> getSubscribeWriteback(
      const moxygen::FullTrackName& ftn,
      std::shared_ptr<moxygen::TrackConsumer> consumer
  );

  std::optional<std::vector<uint64_t>> ingestRelayHopPath(
      const moxygen::PublishNamespace& pubNs,
      const std::shared_ptr<moxygen::MoQSession>& session
  ) const;

  // Result of joinOrPrepareUpstreamSubscription (runs on relayExec_).
  struct StatefulSubscribeResult {
    std::shared_ptr<moxygen::MoQForwarder> publisherForwarder;
    folly::Executor* publisherExec{nullptr}; // owning executor of publisherForwarder
    std::optional<SubscribeResult> error;    // set on failure

    // Set only for the FirstSubscriber path. Consumed by
    // attachNewLocalForwarderOnRelayExec's publisherExec sortie (passive relay chain +
    // upstream subscribe). Pending destructor fires on abandoned move.
    struct FirstSubscriberSetup {
      std::shared_ptr<moxygen::MoQSession> upstreamSession;
      moxygen::SubscribeRequest upstreamSubReq;
      std::shared_ptr<moxygen::TrackConsumer> upstreamConsumer;
      SubscriptionRegistry::UpstreamSubscribePending pending;
      moxygen::RequestID clientRequestID;
    };
    std::optional<FirstSubscriberSetup> firstSetup;
  };

  folly::coro::Task<StatefulSubscribeResult>
  joinOrPrepareUpstreamSubscription(moxygen::SubscribeRequest subReq);

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

  // Output of attachNewLocalForwarderOnRelayExec, read on the subscriberExec tail. The relay
  // chain filter is not exposed (setDownstream/teardown happen inside attach); the tail
  // needs only ownsRelayChain to gate the sawOnEmpty teardown.
  struct PublisherAttachment {
    std::shared_ptr<moxygen::MoQForwarder> publisherFwd;
    folly::Executor* publisherExec{nullptr};
    bool ownsRelayChain{false}; // firstSetup path installed the passive relay chain
    std::shared_ptr<moxygen::MoQForwarder::Callback> finalCallback;
    std::optional<UpstreamOk> upstreamOk;
    std::optional<SubscribeResult> error; // set => bail
  };

  folly::coro::Task<PublisherAttachment> attachNewLocalForwarderOnRelayExec(
      const moxygen::SubscribeRequest& subReq,
      LocalForwarderRegistry* localReg,
      std::shared_ptr<moxygen::MoQForwarder> localFwd,
      folly::Executor* subscriberExec,
      std::shared_ptr<CrossExecFilter> crossExecFilter,
      bool forward
  );

  folly::coro::Task<SubscribeResult> subscribeFromSubscriberExec(
      moxygen::SubscribeRequest subReq,
      std::shared_ptr<moxygen::TrackConsumer> consumer,
      std::shared_ptr<moxygen::MoQSession> session,
      folly::Executor* subscriberExec
  );

  // Answers from this thread's local forwarder (race-free); nullopt defers to trackStatusImpl.
  std::optional<moxygen::Publisher::TrackStatusResult>
  trackStatusOnSubscriberExec(const moxygen::TrackStatus& req);

  // Resolves a joining fetch against this thread's local forwarder (race-free). Rewrites to a
  // standalone Fetch when largest is known, else clears joiningRequestID to defer to upstream.
  moxygen::Fetch
  fetchOnSubscriberExec(moxygen::Fetch fetch, const std::shared_ptr<moxygen::MoQSession>& session);

  // Impl methods — run on relayExec_ when set, or inline when relayExec_==nullptr.
  folly::coro::Task<SubscribeResult>
  subscribeImpl(moxygen::SubscribeRequest subReq, std::shared_ptr<moxygen::TrackConsumer> consumer);
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
  // Must be scheduled on the publisher exec; looks the forwarder up in tlForwarders_ by FTN.
  // nullopt means no active sub, defer upstream.
  folly::coro::Task<std::optional<moxygen::TrackStatusOk>>
  readPublisherForwarderStatus(bool hasHandle, moxygen::TrackStatus req);
  folly::coro::Task<void> onUpstreamConnectImpl(std::shared_ptr<moxygen::MoQSession> session);

  // Synchronous result of publishWithSession: the consumer the publisher writes
  // to and the PublishOk to return to the publisher.  Returned synchronously so
  // the reply coro (coPublish) can co_return the PublishOk immediately after
  // setup without waiting for any downstream peer handshake.
  struct PublishSetup {
    std::shared_ptr<moxygen::TrackConsumer> consumer;
    moxygen::PublishOk publishOk;
  };
  using PublishSetupResult = folly::Expected<PublishSetup, moxygen::PublishError>;

  // Contains all the inline publish() logic, taking session explicitly so it
  // can be called from either the I/O thread (relayExec_==nullptr) or from
  // coPublish on relay exec (where getRequestSession() would return null).
  // If forwarder is non-null it is used as-is (pre-created by publish()); otherwise
  // a new forwarder is created from pub (single-threaded or subscribeNamespace path).
  PublishSetupResult publishWithSession(
      moxygen::PublishRequest pub,
      std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle,
      std::shared_ptr<moxygen::MoQSession> session,
      std::shared_ptr<moxygen::MoQForwarder> forwarder = nullptr
  );

  std::shared_ptr<folly::Executor> ownedRelayExec_;
  folly::Executor* relayExec_{nullptr};
  // Only set in single-threaded mode (relayExec_ == null); used as the
  // coroutine start executor for fire-and-forget tasks like doSubscribeUpdate.
  folly::Executor* sessionExec_{nullptr};

  void maybeSetSessionExec(moxygen::MoQSession& session) {
    if (!relayExec_ && !sessionExec_) {
      sessionExec_ = session.getExecutor();
    }
  }

  folly::Executor* relayExec() const { return relayExec_ ? relayExec_ : sessionExec_; }

  // The relay's execution mode, derived from relayExec_/useLocalForwarders_:
  //   SingleThread   - relayExec_ == nullptr: everything inline on the I/O thread.
  //   RelayExec      - relayExec_ set, useLocalForwarders_ == false: relay state
  //                    isolated on relayExec_; sessions hop via cross-exec filters.
  //   LocalForwarder - relayExec_ set, useLocalForwarders_ == true: per-thread
  //                    local forwarders shortcut the data plane.
  enum class Mode { SingleThread, RelayExec, LocalForwarder };
  Mode mode() const {
    if (!relayExec_) {
      return Mode::SingleThread;
    }
    return useLocalForwarders_ ? Mode::LocalForwarder : Mode::RelayExec;
  }

  std::shared_ptr<moxygen::Publisher> findUpstreamPublisher(const moxygen::TrackNamespace& ns) {
    auto session = namespaceTree_.findPublisherSession(ns);
    if (!session) {
      return nullptr;
    }
    return maybeWrapPublisher(relayExec_, std::move(session));
  }

  bool useLocalForwarders_{false};
  folly::ThreadLocalPtr<LocalForwarderRegistry> tlForwarders_;
  std::unique_ptr<MoqxCache> cache_;
  uint64_t maxDeselected_{kDefaultMaxDeselected};

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
