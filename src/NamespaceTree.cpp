/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "NamespaceTree.h"
#include "relay/ClusterPath.h"
#include <folly/logging/xlog.h>

#include <algorithm>
#include <tuple>

#include "relay/SessionConfined.h"

using namespace moxygen;

namespace openmoq::moqx {

void NamespaceTree::incrementActiveChildren(NamespaceNode& node) {
  node.activeChildCount_++;
  // Propagate up if this was the first active child and the parent itself has
  // no local content (otherwise the parent is already counted).
  if (node.activeChildCount_ == 1 && node.parent_ && !node.hasContent()) {
    incrementActiveChildren(*node.parent_);
  }
}

// Walk up the tree to find and prune the highest empty ancestor.
void NamespaceTree::tryPruneChild(NamespaceNode& parentNode, const std::string& childKey) {
  auto it = parentNode.children_.find(childKey);
  if (it == parentNode.children_.end()) {
    return;
  }

  auto* childNode = it->second.get();
  if (shouldKeep(*childNode)) {
    return;
  }

  // Walk up, decrementing counts, to find the highest empty ancestor to remove.
  std::string keyToRemove = childKey;
  NamespaceNode* parentOfNodeToRemove = &parentNode;
  NamespaceNode* current = &parentNode;

  while (current) {
    XCHECK_GT(current->activeChildCount_, 0);
    current->activeChildCount_--;

    if (current->hasContent() || current->activeChildCount_ > 0) {
      break;
    }

    if (!current->parent_) {
      break; // root — can't remove
    }

    for (const auto& [key, node] : current->parent_->children_) {
      if (node.get() == current) {
        keyToRemove = key;
        parentOfNodeToRemove = current->parent_;
        break;
      }
    }

    current = current->parent_;
  }

  XLOG(DBG1) << "Pruning empty subtree at: " << keyToRemove;
  parentOfNodeToRemove->children_.erase(keyToRemove);
}

void NamespaceTree::notifyParentIfFirstContent(NamespaceNode& node, bool wasEmpty) {
  if (wasEmpty && node.hasContent() && node.parent_) {
    incrementActiveChildren(*node.parent_);
  }
}

void NamespaceTree::tryPruneSelf(NamespaceNode& node, bool hadContent, const TrackNamespace& ns) {
  if (hadContent && !shouldKeep(node) && node.parent_ && !ns.trackNamespace.empty()) {
    tryPruneChild(*node.parent_, ns.trackNamespace.back());
  }
}

bool NamespaceTree::NamespaceNode::betterRoute(const Route& a, const Route& b) {
  auto hasAnon = [](const Route& r) {
    return std::find(r.path.begin(), r.path.end(), kMoQClusterAnonHopId) != r.path.end();
  };
  return std::tuple(hasAnon(a), a.cost, a.path.size(), b.seq) <
         std::tuple(hasAnon(b), b.cost, b.path.size(), a.seq);
}

const NamespaceTree::NamespaceNode* NamespaceTree::findRouteNode(const TrackNamespace& ns) const {
  // publish()/subscribeNamespace() create nodes with no routes; don't let
  // those shadow a publishing ancestor further up the path.
  const NamespaceNode* deepest = root_.routes_.empty() ? nullptr : &root_;
  const NamespaceNode* node = &root_;
  for (size_t i = 0; i < ns.size(); i++) {
    auto it = node->children_.find(ns[i]);
    if (it == node->children_.end()) {
      break;
    }
    node = it->second.get();
    if (!node->routes_.empty()) {
      deepest = node;
    }
  }
  return deepest;
}

std::optional<Participant>
NamespaceTree::findPublisher(const TrackNamespace& ns, const RouteExclusion& exclusion) {
  const auto* node = findRouteNode(ns);
  if (!node) {
    return std::nullopt;
  }
  const auto* route = node->selectRoute([&](const NamespaceNode::Route& r) {
    return (!exclusion.session || r.participant.info->sessionId != *exclusion.session) &&
           !clusterPathContains(r.path, exclusion.hop);
  });
  return route ? std::optional(route->participant) : std::nullopt;
}

const NamespaceTree::NamespaceNode::Route*
NamespaceTree::NamespaceNode::findRoute(SessionId sessionId) const {
  auto it = std::find_if(routes_.begin(), routes_.end(), [&](const Route& r) {
    return r.participant.info->sessionId == sessionId;
  });
  return it != routes_.end() ? &*it : nullptr;
}

std::vector<const NamespaceTree::NamespaceNode::Route*>
NamespaceTree::NamespaceNode::rankedRoutes() const {
  std::vector<const Route*> ranked;
  ranked.reserve(routes_.size());
  for (const auto& route : routes_) {
    ranked.push_back(&route);
  }
  std::sort(ranked.begin(), ranked.end(), [](const Route* a, const Route* b) {
    return betterRoute(*a, *b);
  });
  return ranked;
}

namespace {
// One advertisement keeps its first Hop ID, even 0: its stream is the continuity.
bool sameOrigin(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b) {
  return !a.empty() && !b.empty() && a.front() == b.front();
}
} // namespace

void NamespaceTree::appendSubscribers(const NamespaceNode& node, SubscriberList& out) {
  for (const auto& [_, entry] : node.subscribers_) {
    out.emplace_back(entry.participant, entry.info);
  }
}

void NamespaceTree::reviseRoute(
    NamespaceNode::Route& route,
    std::vector<uint64_t> path,
    uint64_t cost,
    SetPublisherResult& result
) {
  if (!sameOrigin(route.path, path)) {
    route.seq = ++nextRouteSeq_;
    result.replacedPublisher = route.participant.info->sessionId;
  }
  route.path = std::move(path);
  route.cost = cost;
}

NamespaceTree::SetPublisherResult NamespaceTree::setPublisher(
    const TrackNamespace& ns,
    Participant publisher,
    std::shared_ptr<Subscriber::PublishNamespaceCallback> callback,
    std::string peerID,
    RequestID requestID,
    std::vector<uint64_t> relayHopPath,
    uint64_t routeCost,
    bool fromPeering
) {
  SetPublisherResult result;
  auto node = findNode(ns, /*createMissingNodes=*/true, &result.subscribers);
  appendSubscribers(*node, result.subscribers);
  result.node = node;

  const auto session = publisher.info->sessionId;
  auto existing = std::find_if(node->routes_.begin(), node->routes_.end(), [&](const auto& r) {
    return r.participant.info->sessionId == session || (!fromPeering && !r.fromPeering);
  });
  // A NAMESPACE (the only advertisement without a callback) repeated by its session.
  if (existing != node->routes_.end() && existing->participant.info->sessionId == session &&
      fromPeering && !existing->callback && !callback) {
    existing->peerID = std::move(peerID);
    reviseRoute(*existing, std::move(relayHopPath), routeCost, result);
    return result;
  }

  NodeMutationGuard guard(*this, *node, ns);
  if (existing != node->routes_.end()) {
    result.replacedPublisher = existing->participant.info->sessionId;
    if (existing->callback) {
      existing->callback->publishNamespaceCancel(
          PublishNamespaceErrorCode::CANCELLED,
          "New publisher"
      );
    }
    node->routes_.erase(existing);
  }
  node->routes_.push_back(NamespaceNode::Route{
      .participant = std::move(publisher),
      .callback = std::move(callback),
      .peerID = std::move(peerID),
      .requestID = requestID,
      .path = std::move(relayHopPath),
      .cost = routeCost,
      .seq = ++nextRouteSeq_,
      .fromPeering = fromPeering,
  });
  node->trackNamespace = ns;
  return result;
}

folly::Expected<NamespaceTree::SetPublisherResult, NamespaceTree::Error> NamespaceTree::updateRoute(
    const TrackNamespace& ns,
    SessionId sessionId,
    RequestID requestID,
    std::vector<uint64_t> path,
    uint64_t cost
) {
  auto node = findNode(ns);
  if (!node) {
    return folly::makeUnexpected(Error::NodeNotFound);
  }
  auto it = std::find_if(node->routes_.begin(), node->routes_.end(), [&](const auto& r) {
    return r.participant.info->sessionId == sessionId && r.requestID == requestID;
  });
  if (it == node->routes_.end()) {
    return folly::makeUnexpected(Error::NotOwner);
  }
  SetPublisherResult result;
  findNode(ns, /*createMissingNodes=*/false, &result.subscribers);
  appendSubscribers(*node, result.subscribers);
  reviseRoute(*it, std::move(path), cost, result);
  result.node = std::move(node);
  return result;
}

folly::Expected<NamespaceTree::UnpublishNamespaceResult, NamespaceTree::Error>
NamespaceTree::unpublishNamespace(
    const TrackNamespace& ns,
    SessionId sessionId,
    std::optional<RequestID> requestID
) {
  auto node = findNode(ns);
  if (!node) {
    return folly::makeUnexpected(Error::NodeNotFound);
  }
  auto it = std::find_if(node->routes_.begin(), node->routes_.end(), [&](const auto& r) {
    return r.participant.info->sessionId == sessionId && (!requestID || r.requestID == *requestID);
  });
  if (it == node->routes_.end()) {
    return folly::makeUnexpected(Error::NotOwner);
  }

  UnpublishNamespaceResult result;
  findNode(ns, /*createMissingNodes=*/false, &result.subscribers);
  appendSubscribers(*node, result.subscribers);

  NodeMutationGuard guard(*this, *node, ns);
  result.removed = std::move(*it);
  result.removed.callback.reset();
  node->routes_.erase(it);
  result.node = node;
  return result;
}

folly::Expected<folly::Unit, NamespaceTree::Error>
NamespaceTree::unpublishTrack(const TrackNamespace& ns, const std::string& trackName) {
  auto node = findNode(ns);
  if (!node) {
    return folly::makeUnexpected(Error::NodeNotFound);
  }
  NodeMutationGuard guard(*this, *node, ns);
  node->publishes_.erase(trackName);
  return folly::unit;
}

NamespaceTree::AddPublishResult
NamespaceTree::addPublish(const FullTrackName& ftn, Participant publisher, OnRankingFn onRanking) {
  AddPublishResult result;
  result.node = findNode(ftn.trackNamespace, /*createMissingNodes=*/true, &result.subscribers);
  appendSubscribers(*result.node, result.subscribers);
  {
    NodeMutationGuard guard(*this, *result.node, ftn.trackNamespace);
    result.node->publishes_.insert_or_assign(ftn.trackName, std::move(publisher));
  }
  if (onRanking) {
    for (const NamespaceNode* node = result.node.get(); node != nullptr; node = node->parent_) {
      for (const auto& [propertyType, ranking] : node->rankings_) {
        if (ranking) {
          onRanking(propertyType, ranking);
        }
      }
    }
  }
  return result;
}

std::shared_ptr<PropertyRanking>&
NamespaceTree::getOrInsertRanking(NamespaceNode& node, uint64_t propertyType) {
  return node.rankings_[propertyType];
}

std::shared_ptr<NamespaceTree::NamespaceNode> NamespaceTree::addNamespaceSubscriber(
    const TrackNamespace& ns,
    Participant subscriber,
    NamespaceNode::NamespaceSubscriberInfo info
) {
  auto node = findNode(ns, /*createMissingNodes=*/true);
  NodeMutationGuard guard(*this, *node, ns);
  const auto session = subscriber.info->sessionId;
  node->subscribers_.emplace(
      session,
      NamespaceNode::SubscriberEntry{std::move(subscriber), std::move(info)}
  );
  return node;
}

folly::Expected<folly::Unit, NamespaceTree::Error>
NamespaceTree::removeNamespaceSubscriber(const TrackNamespace& ns, SessionId session) {
  auto node = findNode(ns);
  if (!node) {
    return folly::makeUnexpected(Error::NodeNotFound);
  }
  auto it = node->subscribers_.find(session);
  if (it == node->subscribers_.end()) {
    return folly::makeUnexpected(Error::NotSubscribed);
  }
  const auto& trackFilter = it->second.info.trackFilter;
  if (trackFilter) {
    auto rankingIt = node->rankings_.find(trackFilter->propertyType);
    if (rankingIt != node->rankings_.end()) {
      rankingIt->second->removeSubscriberFromTopNGroup(trackFilter->maxSelected, session);
    }
  }
  const NamespaceNode::AdvertisementKey key{session, ns.size()};
  forEachNodeInSubtree(ns, node, [&](const TrackNamespace&, const auto& n) {
    n->eraseSubscriberAdvertisement(key);
  });
  NodeMutationGuard guard(*this, *node, ns);
  node->subscribers_.erase(it);
  return folly::unit;
}

bool NamespaceTree::hasTracksSubscriptionInSubtree(
    const NamespaceNode& root,
    moxygen::SessionId session
) const {
  std::vector<const NamespaceNode*> nodesToVisit{&root};
  while (!nodesToVisit.empty()) {
    const auto* node = nodesToVisit.back();
    nodesToVisit.pop_back();
    if (node->subscribers_.find(session) != node->subscribers_.end()) {
      return true;
    }
    for (const auto& [_, child] : node->children_) {
      nodesToVisit.push_back(child.get());
    }
  }
  return false;
}

bool NamespaceTree::hasOverlappingTracksSubscription(
    const moxygen::TrackNamespace& trackNamespacePrefix,
    moxygen::SessionId session
) const {
  const NamespaceNode* node = &root_;
  for (size_t i = 0; i < trackNamespacePrefix.size(); i++) {
    // Nodes visited before the target prefix are strict ancestors.
    if (node->subscribers_.find(session) != node->subscribers_.end()) {
      return true;
    }
    auto it = node->children_.find(trackNamespacePrefix[i]);
    if (it == node->children_.end()) {
      return false;
    }
    node = it->second.get();
  }
  // The target subtree covers exact-prefix and descendant registrations.
  return hasTracksSubscriptionInSubtree(*node, session);
}

std::shared_ptr<NamespaceTree::NamespaceNode> NamespaceTree::findNode(
    const TrackNamespace& ns,
    bool createMissingNodes,
    SubscriberList* subscribers
) {
  std::shared_ptr<NamespaceNode> nodePtr(std::shared_ptr<void>(), &root_);
  TrackNamespace partialNs;
  for (auto i = 0ul; i < ns.size(); i++) {
    if (subscribers) {
      appendSubscribers(*nodePtr, *subscribers);
    }
    auto& name = ns[i];
    partialNs.append(name);
    auto it = nodePtr->children_.find(name);
    if (it == nodePtr->children_.end()) {
      if (createMissingNodes) {
        auto node = std::make_shared<NamespaceNode>(nodePtr.get());
        node->trackNamespace = partialNs;
        nodePtr->children_.emplace(name, node);
        nodePtr = std::move(node);
      } else {
        XLOG(DBG1) << "namespace node not found: " << ns;
        return nullptr;
      }
    } else {
      nodePtr = it->second;
    }
  }
  return nodePtr;
}

} // namespace openmoq::moqx
