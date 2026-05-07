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

std::shared_ptr<MoQSession> NamespaceTree::findPublisherSession(const TrackNamespace& ns) {
  auto nodePtr = findNode(ns, /*createMissingNodes=*/false, MatchType::Prefix);
  return nodePtr ? nodePtr->publisherSession_ : nullptr;
}

NamespaceTree::SetPublisherResult NamespaceTree::setPublisher(
    const TrackNamespace& ns,
    std::shared_ptr<MoQSession> session,
    std::shared_ptr<Subscriber::PublishNamespaceCallback> callback,
    std::string peerID,
    RequestID requestID
) {
  SetPublisherResult result;
  auto node = findNode(ns, /*createMissingNodes=*/true, MatchType::Exact, &result.subscribers);

  if (node->publisherSession_) {
    result.replacedSession = node->publisherSession_;
    if (node->publishNamespaceCallback_) {
      node->publishNamespaceCallback_->publishNamespaceCancel(
          PublishNamespaceErrorCode::CANCELLED,
          "New publisher"
      );
      node->publishNamespaceCallback_.reset();
    }
    node->publisherSession_.reset();
  }

  for (const auto& [sess, info] : node->subscribers_) {
    result.subscribers.emplace_back(sess, info);
  }

  {
    NodeMutationGuard guard(*this, *node, ns);
    node->publisherSession_ = std::move(session);
    node->publisherPeerID_ = std::move(peerID);
    node->publishNamespaceCallback_ = std::move(callback);
    node->trackNamespace = ns;
    node->setPublishNamespaceOk({.requestID = requestID, .requestSpecificParams = {}});
  }

  result.node = std::move(node);
  return result;
}

folly::Expected<NamespaceTree::UnpublishNamespaceResult, NamespaceTree::Error>
NamespaceTree::unpublishNamespace(
    const TrackNamespace& ns,
    const std::shared_ptr<MoQSession>& session
) {
  auto node = findNode(ns);
  if (!node) {
    return folly::makeUnexpected(Error::NodeNotFound);
  }
  if (node->publisherSession_ == nullptr || node->publisherSession_ != session) {
    return folly::makeUnexpected(Error::NotOwner);
  }

  UnpublishNamespaceResult result;
  findNode(ns, /*createMissingNodes=*/false, MatchType::Exact, &result.subscribers);
  for (const auto& [sess, info] : node->subscribers_) {
    result.subscribers.emplace_back(sess, info);
  }
  for (auto& [sess, handle] : node->draft14PubNsHandles_) {
    result.legacyHandles.emplace_back(sess, handle);
  }

  NodeMutationGuard guard(*this, *node, ns);
  node->publisherSession_ = nullptr;
  node->publisherPeerID_.clear();
  node->publishNamespaceCallback_.reset();
  node->draft14PubNsHandles_.clear();

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
  result.node = findNode(
      ftn.trackNamespace,
      /*createMissingNodes=*/true,
      MatchType::Exact,
      &result.subscribers
  );
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

std::shared_ptr<NamespaceTree::NamespaceNode> NamespaceTree::findNode(
    const TrackNamespace& ns,
    bool createMissingNodes,
    MatchType matchType,
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
      } else if (matchType == MatchType::Prefix && nodePtr.get() != &root_) {
        return nodePtr;
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
