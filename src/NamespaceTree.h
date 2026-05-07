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
#include <moxygen/MoQSession.h>
#include <moxygen/MoQTypes.h>

#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace openmoq::moqx {

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
      return !publishes_.empty() || !subscribers_.empty() || !draft14PubNsHandles_.empty() ||
             publisherSession_ != nullptr;
    }

    using moxygen::Subscriber::PublishNamespaceHandle::setPublishNamespaceOk;

    struct NamespaceSubscriberInfo {
      bool forward{true};
      moxygen::SubscribeNamespaceOptions options{moxygen::SubscribeNamespaceOptions::BOTH};
      // Handle for NAMESPACE/NAMESPACE_DONE on the bidi stream (draft 16+); null for draft <= 15.
      std::shared_ptr<moxygen::Publisher::NamespacePublishHandle> namespacePublishHandle;
      moxygen::TrackNamespace trackNamespacePrefix;
      std::optional<moxygen::TrackFilter> trackFilter;
    };

    std::shared_ptr<moxygen::MoQSession> publisherSession() const { return publisherSession_; }
    const std::string& publisherPeerID() const { return publisherPeerID_; }

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

    void addDraft14PublishNamespaceHandle(
        std::shared_ptr<moxygen::MoQSession> session,
        std::shared_ptr<moxygen::Subscriber::PublishNamespaceHandle> handle
    ) {
      draft14PubNsHandles_.emplace(std::move(session), std::move(handle));
    }

  private:
    friend class NamespaceTree;

    NamespaceTree& tree_;
    NamespaceNode* parent_{nullptr};
    size_t activeChildCount_{0};
    std::shared_ptr<moxygen::MoQSession> publisherSession_;
    std::string publisherPeerID_;
    folly::F14FastMap<std::string, std::shared_ptr<moxygen::MoQSession>> publishes_;
    folly::F14FastMap<std::shared_ptr<moxygen::MoQSession>, NamespaceSubscriberInfo> subscribers_;
    folly::F14FastMap<std::string, std::shared_ptr<NamespaceNode>> children_;
    std::shared_ptr<moxygen::Subscriber::PublishNamespaceCallback> publishNamespaceCallback_;
    folly::F14FastMap<uint64_t, std::shared_ptr<PropertyRanking>> rankings_;
    // Per-subscriber handles for draft<=14 (one per publishNamespace() call), keyed by session.
    folly::F14FastMap<
        std::shared_ptr<moxygen::MoQSession>,
        std::shared_ptr<moxygen::Subscriber::PublishNamespaceHandle>>
        draft14PubNsHandles_;
  };

  using SessionSubscriberList = std::vector<
      std::pair<std::shared_ptr<moxygen::MoQSession>, NamespaceNode::NamespaceSubscriberInfo>>;

  using LegacyDoneHandleList = std::vector<std::pair<
      std::shared_ptr<moxygen::MoQSession>,
      std::shared_ptr<moxygen::Subscriber::PublishNamespaceHandle>>>;

  struct UnpublishNamespaceResult {
    SessionSubscriberList subscribers;
    LegacyDoneHandleList legacyHandles;
  };

  explicit NamespaceTree(Callback& cb) : cb_(cb), root_(*this) {}

  enum class MatchType { Exact, Prefix };
  enum class Error { NodeNotFound, NotOwner, NotSubscribed };

  // Longest-prefix match for the publisher of ns; null if none found.
  std::shared_ptr<moxygen::MoQSession> findPublisherSession(const moxygen::TrackNamespace& ns);

  std::shared_ptr<NamespaceNode> findNode(
      const moxygen::TrackNamespace& ns,
      bool createMissingNodes = false,
      MatchType matchType = MatchType::Exact,
      SessionSubscriberList* subscribers = nullptr
  );

  // Result of setPublisher. node is the target node; subscribers are all
  // prefix + exact sessions; replacedSession is non-null when an existing
  // publisher was displaced (caller must clean up its track subscriptions).
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
      moxygen::RequestID requestID
  );

  // NodeNotFound: node already pruned (ignorable). NotOwner: session mismatch (log and ignore).
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

  // NodeNotFound and NotSubscribed are both ignorable (already gone).
  folly::Expected<folly::Unit, Error> removeNamespaceSubscriber(
      const moxygen::TrackNamespace& ns,
      const std::shared_ptr<moxygen::MoQSession>& session
  );

  // Returns the PropertyRanking slot for propertyType at node, inserting null if absent.
  std::shared_ptr<PropertyRanking>& getOrInsertRanking(NamespaceNode& node, uint64_t propertyType);

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

  void incrementActiveChildren(NamespaceNode& node);
  void tryPruneChild(NamespaceNode& parentNode, const std::string& childKey);

  Callback& cb_;
  NamespaceNode root_{*this};
};

} // namespace openmoq::moqx
