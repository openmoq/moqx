/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Originally from github.com/facebookexperimental/moxygen.
 * See deps/moxygen/LICENSE for the original license terms.
 *
 * Copyright (c) OpenMOQ contributors.
 */

#pragma once

#include "UpstreamProvider.h"
#include "config/Config.h"
#include "relay/PropertyRanking.h"
#include "relay/TopNFilter.h"
#include <folly/coro/SharedPromise.h>
#include <moxygen/MoQSession.h>
#include <moxygen/relay/MoQCache.h>
#include <moxygen/relay/MoQForwarder.h>

#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openmoq::moqx {

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
  // map; empty string for the root node. rankings is non-empty only for nodes
  // that have active PropertyRanking instances (TRACK_FILTER subscribers).
  virtual void beginNamespaceNode(
      std::string_view childKey,
      const moxygen::TrackNamespace& ns,
      size_t sessionCount,
      const std::vector<PropertyRanking::RankingSnapshot>& rankings
  ) = 0;
  virtual void endNamespaceNode() = 0;
  virtual void onNamespaceTreeEnd() = 0;

  // --- Cache section ---
  // Called once with cache state; not called if cache is disabled.
  virtual void onCacheStats(
      size_t totalBytes,
      const std::vector<moxygen::MoQCache::TrackStats>& tracks,
      moxygen::MoQCache::TimePoint now
  ) = 0;
};

class MoqxRelay : public moxygen::Publisher,
                  public moxygen::Subscriber,
                  public std::enable_shared_from_this<MoqxRelay>,
                  public moxygen::MoQForwarder::Callback {
public:
  // Default for maxDeselected (tracks kept in deselected queue before eviction).
  // Set to 0 until pause/resume forwarding callbacks are wired in PropertyRanking;
  // a non-zero value without those callbacks is just topN+N with no benefit.
  static constexpr uint64_t kDefaultMaxDeselected = 0;

  explicit MoqxRelay(
      config::CacheConfig cache = {},
      std::string relayID = {},
      uint64_t maxDeselected = kDefaultMaxDeselected,
      std::chrono::milliseconds idleTimeout = kDefaultIdleTimeout,
      std::chrono::milliseconds activityThreshold = kDefaultActivityThreshold
  )
      : relayID_(std::move(relayID)), maxDeselected_(maxDeselected), idleTimeout_(idleTimeout),
        activityThreshold_(activityThreshold) {
    if (cache.maxCachedTracks > 0) {
      cache_ =
          std::make_unique<moxygen::MoQCache>(cache.maxCachedTracks, cache.maxCachedGroupsPerTrack);
      cache_->setMaxCachedBytes(static_cast<size_t>(cache.maxCachedMb) * 1024 * 1024);
      cache_->setMinEvictionBytes(static_cast<size_t>(cache.minEvictionKb) * 1024);
      cache_->setDefaultMaxCacheDuration(cache.defaultMaxCacheDuration);
      // TODO: wire cache.maxCacheDuration once MoQCache supports clamping
      // publisher-set track durations.
    }
  }

  void setAllowedNamespacePrefix(moxygen::TrackNamespace allowed) {
    allowedNamespacePrefix_ = std::move(allowed);
  }

  // Store the upstream provider. The provider must have been constructed with
  // publishHandler=this and subscribeHandler=this so that the upstream relay's
  // reciprocal subNs and namespace announcements route through MoqxRelay.
  void setUpstreamProvider(std::shared_ptr<UpstreamProvider> upstream) {
    upstream_ = std::move(upstream);
  }

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
  );

  // Wrapper for compatibility - returns single session as vector
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
      std::shared_ptr<moxygen::Subscriber::PublishNamespaceCallback> callback
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
  class TerminationFilter;

  void unsubscribeNamespace(
      const moxygen::TrackNamespace& prefix,
      std::shared_ptr<moxygen::MoQSession> session
  );

  void onPublishDone(const moxygen::FullTrackName& ftn);

  struct NamespaceNode : public moxygen::Subscriber::PublishNamespaceHandle {
    explicit NamespaceNode(MoqxRelay& relay, NamespaceNode* parent = nullptr)
        : relay_(relay), parent_(parent) {}

    void publishNamespaceDone() override { relay_.publishNamespaceDone(trackNamespace_, this); }

    folly::coro::Task<RequestUpdateResult> requestUpdate(moxygen::RequestUpdate reqUpdate
    ) override {
      co_return folly::makeUnexpected(moxygen::RequestError{
          reqUpdate.requestID,
          moxygen::RequestErrorCode::NOT_SUPPORTED,
          "REQUEST_UPDATE not supported for relay PUBLISH_NAMESPACE"
      });
    }

    // Helper to check if THIS node (excluding children) has content
    bool hasLocalSessions() const {
      return !publishes.empty() || !sessions.empty() || !namespacesPublished.empty() ||
             sourceSession != nullptr;
    }

    // Check if node should be kept (has content OR non-empty children)
    bool shouldKeep() const { return hasLocalSessions() || activeChildCount_ > 0; }

    using moxygen::Subscriber::PublishNamespaceHandle::setPublishNamespaceOk;

    moxygen::TrackNamespace trackNamespace_;
    folly::F14FastMap<std::string, std::shared_ptr<NamespaceNode>> children;

    // Maps a track name to a the session performing the PUBLISH
    folly::F14FastMap<std::string, std::shared_ptr<moxygen::MoQSession>> publishes;

    // Info stored per SUBSCRIBE_NAMESPACE subscriber
    struct NamespaceSubscriberInfo {
      bool forward{true};
      moxygen::SubscribeNamespaceOptions options{moxygen::SubscribeNamespaceOptions::BOTH};
      // Handle for sending NAMESPACE / NAMESPACE_DONE on the bidi stream
      // (draft 16+). Null for draft <= 15.
      std::shared_ptr<moxygen::Publisher::NamespacePublishHandle> namespacePublishHandle;
      // The namespace prefix this subscriber used for SUBSCRIBE_NAMESPACE
      moxygen::TrackNamespace trackNamespacePrefix;
      // TRACK_FILTER parameters (non-null when subscriber uses top-N filtering)
      std::optional<moxygen::TrackFilter> trackFilter;
    };

    // PropertyRanking instances per property type for TRACK_FILTER subscribers
    folly::F14FastMap<uint64_t, std::shared_ptr<PropertyRanking>> rankings;

    // Sessions with a SUBSCRIBE_NAMESPACE here, with their preferences
    folly::F14FastMap<std::shared_ptr<moxygen::MoQSession>, NamespaceSubscriberInfo> sessions;
    // All active PUBLISH_NAMESPACEs for this node (includes prefix sessions)
    folly::F14FastMap<std::shared_ptr<moxygen::MoQSession>, std::shared_ptr<PublishNamespaceHandle>>
        namespacesPublished;
    // The session that PUBLISH_NAMESPACEd this node
    std::shared_ptr<moxygen::MoQSession> sourceSession;
    std::shared_ptr<PublishNamespaceCallback> publishNamespaceCallback;

    MoqxRelay& relay_;

    // Pruning support: parent pointer and active child count
    NamespaceNode* parent_{nullptr}; // back link (raw pointer, parent owns us)
    size_t activeChildCount_{0};     // count of children with content

    friend class MoqxRelay;

    void incrementActiveChildren();
    void decrementActiveChildren();
    void tryPruneChild(const std::string& childKey);
  };

  NamespaceNode publishNamespaceRoot_{*this};
  enum class MatchType { Exact, Prefix };
  std::shared_ptr<NamespaceNode> findNamespaceNode(
      const moxygen::TrackNamespace& ns,
      bool createMissingNodes = false,
      MatchType matchType = MatchType::Exact,
      std::vector<
          std::pair<std::shared_ptr<moxygen::MoQSession>, NamespaceNode::NamespaceSubscriberInfo>>*
          sessions = nullptr
  );

  struct RelaySubscription {
    RelaySubscription(
        std::shared_ptr<moxygen::MoQForwarder> f,
        std::shared_ptr<moxygen::MoQSession> u
    )
        : forwarder(std::move(f)), upstream(std::move(u)) {}

    std::shared_ptr<moxygen::MoQForwarder> forwarder;
    std::shared_ptr<moxygen::MoQSession> upstream;
    moxygen::RequestID requestID{0};
    std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle;
    folly::coro::SharedPromise<folly::Unit> promise;
    bool isPublish{false};

    // TopNFilter installed in the publisher's filter chain for property observation
    std::shared_ptr<TopNFilter> topNFilter;

    // Written by TopNFilter on every object arrival; read by PropertyRanking::sweepIdle
    // via the getLastActivity_ callback. Default-constructed (epoch) = always-idle.
    std::chrono::steady_clock::time_point lastObjectTime{};
  };

  void onEmpty(moxygen::MoQForwarder* forwarder) override;
  void forwardChanged(moxygen::MoQForwarder* forwarder) override;
  void newGroupRequested(moxygen::MoQForwarder* forwarder, uint64_t group) override;

  folly::coro::Task<void> publishNamespaceToSession(
      std::shared_ptr<moxygen::MoQSession> session,
      moxygen::PublishNamespace pubNs,
      std::shared_ptr<NamespaceNode> nodePtr
  );

  folly::coro::Task<void> publishToSession(
      std::shared_ptr<moxygen::MoQSession> session,
      std::shared_ptr<moxygen::MoQForwarder> forwarder,
      bool forward,
      bool trackFilterSubscriber = false
  );

  folly::coro::Task<void>
  doSubscribeUpdate(std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle, bool forward);

  folly::coro::Task<void> doNewGroupRequestUpdate(
      std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle,
      uint64_t newGroupRequestValue
  );

  void publishNamespaceDone(const moxygen::TrackNamespace& trackNamespace, NamespaceNode* node);

  // TRACK_FILTER support

  // Result of buildFilterChain - contains both the consumer to pass upstream
  // and the TopNFilter pointer to store for later observer wiring.
  struct FilterChainResult {
    std::shared_ptr<moxygen::TrackConsumer> consumer;
    std::shared_ptr<TopNFilter> topNFilter;
  };

  // Build the filter chain for a track subscription: TopNFilter → TerminationFilter → (cache) →
  // forwarder. Used by both publish() and subscribe() paths to ensure consistent filter chain.
  FilterChainResult buildFilterChain(
      const moxygen::FullTrackName& ftn,
      std::shared_ptr<moxygen::MoQForwarder> forwarder
  );

  // Get or create PropertyRanking for the given property type on a namespace node.
  // Retroactively registers any tracks already published under that node.
  // ns must be the full namespace of `node` (used as BFS seed for track registration).
  std::shared_ptr<PropertyRanking> getOrCreateRanking(
      std::shared_ptr<NamespaceNode> node,
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
  folly::F14NodeMap<moxygen::FullTrackName, RelaySubscription, moxygen::FullTrackName::hash>
      subscriptions_;

  std::shared_ptr<moxygen::TrackConsumer> getSubscribeWriteback(
      const moxygen::FullTrackName& ftn,
      std::shared_ptr<moxygen::TrackConsumer> consumer
  );
  std::unique_ptr<moxygen::MoQCache> cache_;
  uint64_t maxDeselected_{kDefaultMaxDeselected};

  static constexpr std::chrono::milliseconds kDefaultIdleTimeout{10'000};
  static constexpr std::chrono::milliseconds kDefaultActivityThreshold{2'000};
  std::chrono::milliseconds idleTimeout_{kDefaultIdleTimeout};
  std::chrono::milliseconds activityThreshold_{kDefaultActivityThreshold};
};

// Creates a NamespacePublishHandle that bridges NAMESPACE/NAMESPACE_DONE
// messages from a peer relay into relay->doPublishNamespace() synchronously.
// Used for both the initiating (UpstreamProvider) and reciprocal (MoqxRelay) paths.
std::shared_ptr<moxygen::Publisher::NamespacePublishHandle> makeNamespaceBridgeHandle(
    std::weak_ptr<MoqxRelay> relay,
    std::shared_ptr<moxygen::MoQSession> session
);

} // namespace openmoq::moqx
