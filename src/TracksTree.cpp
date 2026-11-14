/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TracksTree.h"

namespace openmoq::moqx {

TracksTree::Node* TracksTree::addSubscriber(
    const moxygen::TrackNamespace& prefix,
    Participant subscriber,
    TrackSubscriberInfo info
) {
  Node* node = &root_;
  for (size_t i = 0; i < prefix.size(); i++) {
    auto& child = node->children_[prefix[i]];
    if (!child) {
      child.reset(new Node(node));
    }
    node = child.get();
  }
  const Node::Key key{subscriber.info->sessionId, info.source};
  const bool inserted =
      node->subscribers_.emplace(key, Node::Entry{std::move(subscriber), std::move(info)}).second;
  return inserted ? node : nullptr;
}

bool TracksTree::removeSubscriber(
    const moxygen::TrackNamespace& prefix,
    moxygen::SessionId session,
    TrackSubscriptionSource source
) {
  Node* node = find(prefix);
  if (!node) {
    return false;
  }
  auto it = node->subscribers_.find(Node::Key{session, source});
  if (it == node->subscribers_.end()) {
    return false;
  }
  if (const auto& filter = it->second.info.trackFilter) {
    if (auto rankingIt = node->rankings_.find(filter->propertyType);
        rankingIt != node->rankings_.end() && rankingIt->second) {
      rankingIt->second->removeSubscriberFromTopNGroup(filter->maxSelected, session);
    }
  }
  node->subscribers_.erase(it);
  prune(node);
  return true;
}

TracksTree::SubscriberList TracksTree::subscribersCovering(const moxygen::TrackNamespace& ns
) const {
  SubscriberList out;
  const Node* node = &root_;
  for (size_t i = 0;; i++) {
    for (const auto& [_, entry] : node->subscribers_) {
      out.emplace_back(entry.participant, entry.info);
    }
    if (i == ns.size()) {
      return out;
    }
    auto it = node->children_.find(ns[i]);
    if (it == node->children_.end()) {
      return out;
    }
    node = it->second.get();
  }
}

bool TracksTree::hasOverlap(const moxygen::TrackNamespace& prefix, moxygen::SessionId session)
    const {
  const Node::Key key{session, TrackSubscriptionSource::SubscribeTracks};
  const Node* node = &root_;
  for (size_t i = 0; i < prefix.size(); i++) {
    if (node->subscribers_.contains(key)) {
      return true;
    }
    auto it = node->children_.find(prefix[i]);
    if (it == node->children_.end()) {
      return false;
    }
    node = it->second.get();
  }
  return subtreeHas(*node, key);
}

bool TracksTree::hasSubscriber(
    const moxygen::TrackNamespace& prefix,
    moxygen::SessionId session,
    TrackSubscriptionSource source
) const {
  const Node* node = find(prefix);
  return node && node->subscribers_.contains(Node::Key{session, source});
}

TracksTree::Node* TracksTree::find(const moxygen::TrackNamespace& prefix) const {
  const Node* node = &root_;
  for (size_t i = 0; i < prefix.size(); i++) {
    auto it = node->children_.find(prefix[i]);
    if (it == node->children_.end()) {
      return nullptr;
    }
    node = it->second.get();
  }
  return const_cast<Node*>(node);
}

bool TracksTree::subtreeHas(const Node& node, const Node::Key& key) {
  if (node.subscribers_.contains(key)) {
    return true;
  }
  for (const auto& [_, child] : node.children_) {
    if (subtreeHas(*child, key)) {
      return true;
    }
  }
  return false;
}

void TracksTree::prune(Node* node) {
  while (node->parent_ && node->subscribers_.empty() && node->children_.empty()) {
    Node* parent = node->parent_;
    for (auto it = parent->children_.begin(); it != parent->children_.end(); ++it) {
      if (it->second.get() == node) {
        parent->children_.erase(it);
        break;
      }
    }
    node = parent;
  }
  // A node with no subscribers keeps no rankings.
  if (node->subscribers_.empty()) {
    node->rankings_.clear();
  }
}

} // namespace openmoq::moqx
