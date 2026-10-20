/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "relay/PropertyRanking.h"
#include <folly/Expected.h>
#include <folly/Unit.h>
#include <folly/container/F14Map.h>
#include <folly/hash/Hash.h>
#include <moxygen/MoQSession.h>
#include <moxygen/MoQTypes.h>

#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace openmoq::moqx {

// Routes a request may not use.
struct RouteExclusion {
  std::optional<moxygen::SessionId> session;
  uint64_t hop{moxygen::kMoQClusterAnonHopId};
};

// Hierarchical tree tracking PUBLISH_NAMESPACE announcements and
// SUBSCRIBE_NAMESPACE subscriptions. Relay-specific notifications are
// delivered via Callback.
class NamespaceTree {
public:
  struct Callback {
    virtual ~Callback() = default;
    // Fired when a node's publishNamespaceDone() fires; caller must identify
    // the session via MoQSession::getRequestSession().
    virtual void onPublishNamespaceDone(const moxygen::TrackNamespace& ns) = 0;
  };

  struct NamespaceNode : public moxygen::Subscriber::PublishNamespaceHandle {
    explicit NamespaceNode(NamespaceTree& tree, NamespaceNode* parent = nullptr)
        : tree_(tree), parent_(parent) {}

    moxygen::TrackNamespace trackNamespace;

    void publishNamespaceDone() override { tree_.onNodeDone(trackNamespace); }

    folly::coro::Task<RequestUpdateResult> requestUpdate(moxygen::RequestUpdate reqUpdate
    ) override {
      co_return folly::makeUnexpected(moxygen::RequestError{
          reqUpdate.requestID,
          moxygen::RequestErrorCode::NOT_SUPPORTED,
          "REQUEST_UPDATE not supported for relay PUBLISH_NAMESPACE"
      });
    }

    bool hasContent() const {
      return !publishes_.empty() || !subscribers_.empty() || !routes_.empty();
    }

    using moxygen::Subscriber::PublishNamespaceHandle::setPublishNamespaceOk;

    struct NamespaceSubscriberInfo {
      bool forward{true};
      moxygen::SubscribeNamespaceOptions options{moxygen::SubscribeNamespaceOptions::BOTH};
      // Handle for NAMESPACE/NAMESPACE_DONE on the bidi stream; null for SUBSCRIBE_TRACKS entries.
      std::shared_ptr<moxygen::Publisher::NamespacePublishHandle> namespacePublishHandle;
      moxygen::TrackNamespace trackNamespacePrefix;
      std::optional<moxygen::TrackFilter> trackFilter;
      std::string peerID; // relay ID of a peer relay subscriber
    };

    // What one SUBSCRIBE_NAMESPACE was last told about this namespace.
    struct Advertisement {
      moxygen::SessionId source;
      std::vector<uint64_t> path;
      uint64_t cost{0};
    };
    // A subscriber session and its prefix length: one session can hold nested prefixes.
    using AdvertisementKey = std::pair<moxygen::SessionId, size_t>;

    const Advertisement* findAdvertisement(const AdvertisementKey& key) const {
      auto it = advertisements_.find(key);
      return it != advertisements_.end() ? &it->second : nullptr;
    }
    void setAdvertisement(const AdvertisementKey& key, Advertisement advertisement) {
      advertisements_.insert_or_assign(key, std::move(advertisement));
    }
    void eraseAdvertisement(const AdvertisementKey& key) { advertisements_.erase(key); }

    // One advertisement of this namespace. A session holds at most one route here.
    struct Route {
      std::shared_ptr<moxygen::MoQSession> session;
      std::shared_ptr<moxygen::Subscriber::PublishNamespaceCallback> callback;
      std::string peerID;
      std::vector<uint64_t> path;
      uint64_t cost{0};        // ROUTE_COST plus the sender's link cost
      uint64_t seq{0};         // arrival order; newer wins ties
      bool fromCluster{false}; // from a session that negotiated MoQ Cluster
    };

    const std::vector<Route>& routes() const { return routes_; }

    // Best eligible route by the MoQ Cluster path-selection rule: a path
    // without 0, then lowest cost, then shortest path, then newest.
    template <typename Pred> const Route* selectRoute(Pred&& eligible) const {
      const Route* best = nullptr;
      for (const auto& route : routes_) {
        if (eligible(route) && (!best || betterRoute(route, *best))) {
          best = &route;
        }
      }
      return best;
    }
    const Route* selectRoute() const {
      return selectRoute([](const Route&) { return true; });
    }

    size_t subscriberCount() const { return subscribers_.size(); }

    std::shared_ptr<moxygen::MoQSession> findPublishSession(const std::string& trackName) const {
      auto it = publishes_.find(trackName);
      return it != publishes_.end() ? it->second : nullptr;
    }

    size_t publishCount() const { return publishes_.size(); }

    template <typename Fn> void forEachPublish(Fn&& fn) const {
      for (const auto& [trackName, session] : publishes_) {
        fn(trackName, session);
      }
    }

    template <typename Fn> void forEachSubscriber(Fn&& fn) const {
      for (const auto& [session, info] : subscribers_) {
        fn(session, info);
      }
    }

  private:
    friend class NamespaceTree;

    NamespaceTree& tree_;
    NamespaceNode* parent_{nullptr};
    size_t activeChildCount_{0};
    static bool betterRoute(const Route& a, const Route& b);

    std::vector<Route> routes_;
    struct AdvertisementKeyHash {
      size_t operator()(const AdvertisementKey& key) const {
        return folly::hash::hash_combine(moxygen::SessionId::hash{}(key.first), key.second);
      }
    };
    folly::F14FastMap<AdvertisementKey, Advertisement, AdvertisementKeyHash> advertisements_;
    folly::F14FastMap<std::string, std::shared_ptr<moxygen::MoQSession>> publishes_;
    folly::F14FastMap<std::shared_ptr<moxygen::MoQSession>, NamespaceSubscriberInfo> subscribers_;
    folly::F14FastMap<std::string, std::shared_ptr<NamespaceNode>> children_;
    folly::F14FastMap<uint64_t, std::shared_ptr<PropertyRanking>> rankings_;
  };

  using SessionSubscriberList = std::vector<
      std::pair<std::shared_ptr<moxygen::MoQSession>, NamespaceNode::NamespaceSubscriberInfo>>;

  struct UnpublishNamespaceResult {
    std::shared_ptr<NamespaceNode> node;
    SessionSubscriberList subscribers;
    NamespaceNode::Route removed;
  };

  explicit NamespaceTree(Callback& cb) : cb_(cb), root_(*this) {}

  enum class Error { NodeNotFound, NotOwner, NotSubscribed };

  // The deepest node covering ns that has routes. Null if none does.
  const NamespaceNode* findRouteNode(const moxygen::TrackNamespace& ns) const;

  // Session of the best route at the deepest node covering ns that has routes.
  // Null if that node has no eligible route: shorter prefixes are not consulted.
  std::shared_ptr<moxygen::MoQSession>
  findPublisherSession(const moxygen::TrackNamespace& ns, const RouteExclusion& exclusion = {});

  std::shared_ptr<NamespaceNode> findNode(
      const moxygen::TrackNamespace& ns,
      bool createMissingNodes = false,
      SessionSubscriberList* subscribers = nullptr
  );

  // Result of setPublisher. node is the target node; subscribers are all
  // prefix + exact sessions; replacedSession is non-null when an existing
  // route was displaced (caller must clean up its track subscriptions). A
  // route that is not fromCluster displaces the node's other such route, and
  // any route displaces the same session's earlier route. Other routes coexist.
  struct SetPublisherResult {
    std::shared_ptr<NamespaceNode> node;
    SessionSubscriberList subscribers;
    std::shared_ptr<moxygen::MoQSession> replacedSession;
  };

  SetPublisherResult setPublisher(
      const moxygen::TrackNamespace& ns,
      std::shared_ptr<moxygen::MoQSession> session,
      std::shared_ptr<moxygen::Subscriber::PublishNamespaceCallback> callback,
      std::string peerID,
      moxygen::RequestID requestID,
      std::vector<uint64_t> relayHopPath = {},
      uint64_t routeCost = 0,
      bool fromCluster = false
  );

  // Removes session's route. NodeNotFound: node already pruned (ignorable).
  // NotOwner: session has no route here (log and ignore).
  folly::Expected<UnpublishNamespaceResult, Error> unpublishNamespace(
      const moxygen::TrackNamespace& ns,
      const std::shared_ptr<moxygen::MoQSession>& session
  );

  // NodeNotFound means the node was already pruned, which is ignorable.
  folly::Expected<folly::Unit, Error>
  unpublishTrack(const moxygen::TrackNamespace& ns, const std::string& trackName);

  struct AddPublishResult {
    std::shared_ptr<NamespaceNode> node;
    SessionSubscriberList subscribers;
  };
  using OnRankingFn = std::function<void(uint64_t, const std::shared_ptr<PropertyRanking>&)>;

  // Records ftn.trackName→session, collects prefix+exact subscribers, and
  // calls onRanking for every non-null PropertyRanking at the node and ancestors.
  AddPublishResult addPublish(
      const moxygen::FullTrackName& ftn,
      std::shared_ptr<moxygen::MoQSession> session,
      OnRankingFn onRanking = {}
  );

  // Returns the node so the caller can wire up PropertyRanking if needed.
  std::shared_ptr<NamespaceNode> addNamespaceSubscriber(
      const moxygen::TrackNamespace& ns,
      std::shared_ptr<moxygen::MoQSession> session,
      NamespaceNode::NamespaceSubscriberInfo info
  );

  // Also forgets what the subscription was told. NodeNotFound and NotSubscribed
  // are both ignorable (already gone).
  folly::Expected<folly::Unit, Error> removeNamespaceSubscriber(
      const moxygen::TrackNamespace& ns,
      const std::shared_ptr<moxygen::MoQSession>& session
  );

  // Returns the PropertyRanking slot for propertyType at node, inserting null if absent.
  std::shared_ptr<PropertyRanking>& getOrInsertRanking(NamespaceNode& node, uint64_t propertyType);

  // Draft 18+ helpers for the SUBSCRIBE_TRACKS parallel tree.
  // Checks whether any existing subscriber entry in this tree for `session`
  // overlaps the given prefix (exact, ancestor, or descendant match).
  bool hasOverlappingTracksSubscription(
      const moxygen::TrackNamespace& trackNamespacePrefix,
      const std::shared_ptr<moxygen::MoQSession>& session
  ) const;

  // DFS walk. begin(childKey, node) is called on entry; end() on exit after
  // all children. childKey is empty for the root.
  template <typename BeginFn, typename EndFn> void walkTree(BeginFn&& begin, EndFn&& end) const {
    std::function<void(std::string_view, const NamespaceNode&)> walk;
    walk = [&](std::string_view key, const NamespaceNode& node) {
      begin(key, node);
      for (const auto& [childKey, child] : node.children_) {
        walk(childKey, *child);
      }
      end();
    };
    walk("", root_);
  }

  // BFS from startNode. Calls fn(prefix, node) for startNode and every descendant.
  template <typename Fn>
  void forEachNodeInSubtree(
      const moxygen::TrackNamespace& startNs,
      std::shared_ptr<NamespaceNode> startNode,
      Fn&& fn
  ) {
    std::deque<std::pair<moxygen::TrackNamespace, std::shared_ptr<NamespaceNode>>> queue;
    queue.emplace_back(startNs, std::move(startNode));
    while (!queue.empty()) {
      auto [ns, node] = std::move(queue.front());
      queue.pop_front();
      fn(ns, node);
      for (auto& [childKey, childNode] : node->children_) {
        moxygen::TrackNamespace childNs(ns);
        childNs.append(childKey);
        queue.emplace_back(std::move(childNs), childNode);
      }
    }
  }

private:
  // RAII guard that snapshots hasContent() at construction and calls
  // notifyParentIfFirstContent + tryPruneSelf on destruction. Bracket any
  // mutation of a node's content fields with this guard.
  class NodeMutationGuard {
  public:
    NodeMutationGuard(NamespaceTree& tree, NamespaceNode& node, moxygen::TrackNamespace ns)
        : tree_(tree), node_(node), ns_(std::move(ns)), hadContent_(node.hasContent()) {}
    ~NodeMutationGuard() {
      tree_.notifyParentIfFirstContent(node_, !hadContent_);
      tree_.tryPruneSelf(node_, hadContent_, ns_);
    }
    NodeMutationGuard(const NodeMutationGuard&) = delete;
    NodeMutationGuard& operator=(const NodeMutationGuard&) = delete;

  private:
    NamespaceTree& tree_;
    NamespaceNode& node_;
    moxygen::TrackNamespace ns_;
    bool hadContent_;
  };

  void onNodeDone(const moxygen::TrackNamespace& ns) { cb_.onPublishNamespaceDone(ns); }

  bool shouldKeep(const NamespaceNode& node) const {
    return node.hasContent() || node.activeChildCount_ > 0;
  }

  // Called after content is added: increments parent's activeChildCount when
  // node transitions from empty to non-empty.
  void notifyParentIfFirstContent(NamespaceNode& node, bool wasEmpty);

  // Called after content is removed: prunes node from its parent if now empty.
  void tryPruneSelf(NamespaceNode& node, bool hadContent, const moxygen::TrackNamespace& ns);

  bool hasTracksSubscriptionInSubtree(
      const NamespaceNode& node,
      const std::shared_ptr<moxygen::MoQSession>& session
  ) const;

  void incrementActiveChildren(NamespaceNode& node);
  void tryPruneChild(NamespaceNode& parentNode, const std::string& childKey);

  Callback& cb_;
  NamespaceNode root_{*this};
  uint64_t nextRouteSeq_{0};
};

} // namespace openmoq::moqx
