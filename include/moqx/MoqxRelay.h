/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Originally from github.com/facebookexperimental/moxygen.
 * See deps/moxygen/LICENSE for the original license terms.
 *
 * Copyright (c) OpenMOQ contributors.
 */

#pragma once

#include <folly/coro/SharedPromise.h>
#include <moqx/UpstreamProvider.h>
#include <moxygen/MoQSession.h>
#include <moxygen/relay/MoQCache.h>
#include <moxygen/relay/MoQForwarder.h>

#include <folly/container/F14Set.h>
#include <string>

namespace openmoq::moqx {

class MoqxRelay : public moxygen::Publisher,
                  public moxygen::Subscriber,
                  public std::enable_shared_from_this<MoqxRelay>,
                  public moxygen::MoQForwarder::Callback {
public:
  explicit MoqxRelay(
      size_t maxCachedTracks = moxygen::kDefaultMaxCachedTracks,
      size_t maxCachedGroupsPerTrack = moxygen::kDefaultMaxCachedGroupsPerTrack,
      std::string relayID = {}
  )
      : relayID_(std::move(relayID)) {
    if (maxCachedTracks > 0) {
      cache_ = std::make_unique<moxygen::MoQCache>(maxCachedTracks, maxCachedGroupsPerTrack);
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
    };

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
  };

  void onEmpty(moxygen::MoQForwarder* forwarder) override;
  void forwardChanged(moxygen::MoQForwarder* forwarder) override;

  folly::coro::Task<void> publishNamespaceToSession(
      std::shared_ptr<moxygen::MoQSession> session,
      moxygen::PublishNamespace pubNs,
      std::shared_ptr<NamespaceNode> nodePtr
  );

  folly::coro::Task<void> publishToSession(
      std::shared_ptr<moxygen::MoQSession> session,
      std::shared_ptr<moxygen::MoQForwarder> forwarder,
      bool forward
  );

  folly::coro::Task<void>
  doSubscribeUpdate(std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle, bool forward);

  void publishNamespaceDone(const moxygen::TrackNamespace& trackNamespace, NamespaceNode* node);

  moxygen::TrackNamespace allowedNamespacePrefix_;
  std::string relayID_;
  std::shared_ptr<UpstreamProvider> upstream_;

  // Holds the peer subNs handle for the upstream (initiating) direction.
  // Kept alive so the subscription is not cancelled when onUpstreamConnect returns.
  std::shared_ptr<moxygen::Publisher::SubscribeNamespaceHandle> upstreamSubNsHandle_;

  // Reciprocal peer subNs handles: one per peer relay session that has
  // connected to us. Kept alive so the subscription is not immediately
  // cancelled. Keyed by raw session pointer (valid for session lifetime).
  folly::F14FastMap<
      moxygen::MoQSession*,
      std::shared_ptr<moxygen::Publisher::SubscribeNamespaceHandle>>
      peerSubNsHandles_;
  folly::F14FastMap<moxygen::FullTrackName, RelaySubscription, moxygen::FullTrackName::hash>
      subscriptions_;

  std::shared_ptr<moxygen::TrackConsumer> getSubscribeWriteback(
      const moxygen::FullTrackName& ftn,
      std::shared_ptr<moxygen::TrackConsumer> consumer
  );
  std::unique_ptr<moxygen::MoQCache> cache_;
};

// Creates a NamespacePublishHandle that bridges NAMESPACE/NAMESPACE_DONE
// messages from a peer relay into relay->doPublishNamespace() synchronously.
// Used for both the initiating (UpstreamProvider) and reciprocal (MoqxRelay) paths.
std::shared_ptr<moxygen::Publisher::NamespacePublishHandle> makeNamespaceBridgeHandle(
    std::weak_ptr<MoqxRelay> relay,
    std::shared_ptr<moxygen::MoQSession> session
);

} // namespace openmoq::moqx
