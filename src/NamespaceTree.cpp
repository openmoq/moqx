/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "NamespaceTree.h"
#include <folly/logging/xlog.h>

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

std::optional<NamespaceTree::SelectedPublisher> NamespaceTree::NamespaceNode::selectPublisher(
    uint64_t excludedHop,
    const std::shared_ptr<MoQSession>& excludedSession
) const {
  uint64_t excludedRoute = 0;
  for (const auto& [id, source] : sources_) {
    if (source.session == excludedSession) {
      excludedRoute = id;
      break;
    }
  }
  const auto* route = routes_.select(excludedHop, excludedRoute);
  if (!route) {
    return std::nullopt;
  }
  return SelectedPublisher{
      route->id,
      sources_.at(route->id).session,
      route->path,
      route->cost,
      route->advertisedCost,
      routes_.contentEpoch(),
      route->received
  };
}

std::optional<NamespaceTree::SelectedPublisher>
NamespaceTree::NamespaceNode::publisherFrom(const std::shared_ptr<MoQSession>& session) const {
  for (const auto& [id, source] : sources_) {
    if (source.session == session) {
      const auto* route = routes_.find(id);
      return SelectedPublisher{
          id,
          session,
          route->path,
          route->cost,
          route->advertisedCost,
          routes_.contentEpoch(),
          route->received
      };
    }
  }
  return std::nullopt;
}

std::vector<NamespaceTree::SelectedPublisher> NamespaceTree::NamespaceNode::publishers() const {
  std::vector<SelectedPublisher> result;
  result.reserve(sources_.size());
  for (const auto& [id, source] : sources_) {
    const auto* route = routes_.find(id);
    result.push_back(SelectedPublisher{
        id,
        source.session,
        route->path,
        route->cost,
        route->advertisedCost,
        routes_.contentEpoch(),
        route->received
    });
  }
  return result;
}

void NamespaceTree::NamespaceNode::refreshPublisher() {
  auto selected = selectPublisher();
  publisherSession_ = selected ? selected->session : nullptr;
  relayHopPath_ = selected ? selected->path : std::vector<uint64_t>{};
  publisherPeerID_ = selected ? sources_.at(selected->routeID).peerID : std::string{};
}

std::shared_ptr<NamespaceTree::NamespaceNode>
NamespaceTree::findPublisherNode(const TrackNamespace& ns) {
  auto node = std::shared_ptr<NamespaceNode>(std::shared_ptr<void>(), &root_);
  auto deepest = root_.routeCount() ? node : nullptr;
  for (size_t i = 0; i < ns.size(); ++i) {
    auto it = node->children_.find(ns[i]);
    if (it == node->children_.end()) {
      break;
    }
    node = it->second;
    if (node->routeCount()) {
      deepest = node;
    }
  }
  return deepest;
}

std::shared_ptr<MoQSession> NamespaceTree::findPublisherSession(
    const TrackNamespace& ns,
    uint64_t excludedHop,
    const std::shared_ptr<MoQSession>& excludedSession
) {
  auto node = findPublisherNode(ns);
  auto selected = node ? node->selectPublisher(excludedHop, excludedSession) : std::nullopt;
  return selected ? selected->session : nullptr;
}

NamespaceTree::SetPublisherResult NamespaceTree::setPublisher(
    const TrackNamespace& ns,
    std::shared_ptr<MoQSession> session,
    std::shared_ptr<Subscriber::PublishNamespaceCallback> callback,
    std::string peerID,
    RequestID requestID,
    std::vector<uint64_t> relayHopPath,
    uint64_t advertisedCost,
    uint64_t linkCost,
    uint64_t routeID
) {
  SetPublisherResult result;
  auto node = findNode(ns, !routeID, &result.subscribers);
  if (routeID) {
    auto current = node ? node->publisherFrom(session) : std::nullopt;
    if (!current || current->routeID != routeID) {
      return result;
    }
  }
  NodeMutationGuard guard(*this, *node, ns);
  auto previous = node->publisherSession_;
  uint64_t oldRouteID = 0;
  if (!routeID) {
    routeID = ++nextRouteID_;
    if (auto existing = node->publisherFrom(session)) {
      oldRouteID = existing->routeID;
    }
  }
  if (relayHopPath.empty()) {
    relayHopPath.push_back(0);
  }
  result.contentChanged =
      node->routes_.update(routeID, std::move(relayHopPath), advertisedCost, linkCost);
  if (oldRouteID) {
    node->routes_.remove(oldRouteID);
  }
  if (result.contentChanged) {
    result.replacedSession = previous;
  }
  std::vector<std::shared_ptr<Subscriber::PublishNamespaceCallback>> cancelled;
  for (auto it = node->sources_.begin(); it != node->sources_.end();) {
    if (!node->routes_.find(it->first)) {
      if (it->second.callback) {
        cancelled.push_back(std::move(it->second.callback));
      }
      it = node->sources_.erase(it);
    } else {
      ++it;
    }
  }
  auto existing = node->sources_.find(routeID);
  if (!callback && existing != node->sources_.end()) {
    callback = existing->second.callback;
  }
  node->sources_.insert_or_assign(
      routeID,
      NamespaceNode::Source{std::move(session), std::move(callback), std::move(peerID), requestID}
  );
  node->refreshPublisher();
  node->trackNamespace = ns;
  node->setPublishNamespaceOk({.requestID = requestID, .requestSpecificParams = {}});
  for (const auto& [sess, info] : node->subscribers_) {
    result.subscribers.emplace_back(sess, info);
  }
  result.node = node;
  result.routeID = routeID;
  for (const auto& cb : cancelled) {
    cb->publishNamespaceCancel(PublishNamespaceErrorCode::CANCELLED, "Publisher replaced");
  }
  return result;
}

folly::Expected<NamespaceTree::UnpublishNamespaceResult, NamespaceTree::Error>
NamespaceTree::unpublishNamespace(
    const TrackNamespace& ns,
    const std::shared_ptr<MoQSession>& session,
    uint64_t routeID
) {
  auto node = findNode(ns);
  if (!node) {
    return folly::makeUnexpected(Error::NodeNotFound);
  }
  auto source = node->publisherFrom(session);
  if (!source || (routeID && source->routeID != routeID)) {
    return folly::makeUnexpected(Error::NotOwner);
  }
  UnpublishNamespaceResult result;
  result.node = node;
  result.relayHopPath = source->path;
  findNode(ns, false, &result.subscribers);
  for (const auto& [sess, info] : node->subscribers_) {
    result.subscribers.emplace_back(sess, info);
  }
  NodeMutationGuard guard(*this, *node, ns);
  node->routes_.remove(source->routeID);
  node->sources_.erase(source->routeID);
  node->refreshPublisher();
  if (!node->routeCount()) {
    for (auto& [sess, handle] : node->draft14PubNsHandles_) {
      result.legacyHandles.emplace_back(sess, handle);
    }
    node->draft14PubNsHandles_.clear();
  }
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

NamespaceTree::AddPublishResult NamespaceTree::addPublish(
    const FullTrackName& ftn,
    std::shared_ptr<MoQSession> session,
    OnRankingFn onRanking
) {
  AddPublishResult result;
  result.node = findNode(ftn.trackNamespace, /*createMissingNodes=*/true, &result.subscribers);
  for (const auto& [sess, info] : result.node->subscribers_) {
    result.subscribers.emplace_back(sess, info);
  }
  {
    NodeMutationGuard guard(*this, *result.node, ftn.trackNamespace);
    result.node->publishes_.insert_or_assign(ftn.trackName, session);
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
    std::shared_ptr<MoQSession> session,
    NamespaceNode::NamespaceSubscriberInfo info
) {
  auto node = findNode(ns, /*createMissingNodes=*/true);
  NodeMutationGuard guard(*this, *node, ns);
  node->subscribers_.emplace(std::move(session), std::move(info));
  return node;
}

folly::Expected<folly::Unit, NamespaceTree::Error> NamespaceTree::removeNamespaceSubscriber(
    const TrackNamespace& ns,
    const std::shared_ptr<MoQSession>& session
) {
  auto node = findNode(ns);
  if (!node) {
    return folly::makeUnexpected(Error::NodeNotFound);
  }
  auto it = node->subscribers_.find(session);
  if (it == node->subscribers_.end()) {
    return folly::makeUnexpected(Error::NotSubscribed);
  }
  if (it->second.trackFilter) {
    auto rankingIt = node->rankings_.find(it->second.trackFilter->propertyType);
    if (rankingIt != node->rankings_.end()) {
      rankingIt->second->removeSessionFromTopNGroup(it->second.trackFilter->maxSelected, session);
    }
  }
  NodeMutationGuard guard(*this, *node, ns);
  node->subscribers_.erase(it);
  return folly::unit;
}

bool NamespaceTree::hasTracksSubscriptionInSubtree(
    const NamespaceNode& root,
    const std::shared_ptr<moxygen::MoQSession>& session
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
    const std::shared_ptr<moxygen::MoQSession>& session
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
    SessionSubscriberList* subscribers
) {
  std::shared_ptr<NamespaceNode> nodePtr(std::shared_ptr<void>(), &root_);
  TrackNamespace partialNs;
  for (auto i = 0ul; i < ns.size(); i++) {
    if (subscribers) {
      for (const auto& [session, info] : nodePtr->subscribers_) {
        subscribers->emplace_back(session, info);
      }
    }
    auto& name = ns[i];
    partialNs.append(name);
    auto it = nodePtr->children_.find(name);
    if (it == nodePtr->children_.end()) {
      if (createMissingNodes) {
        auto node = std::make_shared<NamespaceNode>(*this, nodePtr.get());
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
