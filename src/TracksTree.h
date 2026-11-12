/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "relay/Participant.h"
#include "relay/PropertyRanking.h"
#include <folly/Utility.h>
#include <folly/container/F14Map.h>
#include <folly/hash/Hash.h>
#include <moxygen/MoQTypes.h>
#include <moxygen/openmoq/extensions/TopTracksExtension.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace openmoq::moqx {

// The request a track subscription came from. A session may hold one of each at a prefix.
enum class TrackSubscriptionSource : uint8_t {
  SubscribeNamespace,
  SubscribeTracks,
};

struct TrackSubscriberInfo {
  TrackSubscriptionSource source{TrackSubscriptionSource::SubscribeTracks};
  bool forward{true};
  // Set for a top-N subscriber, which gets tracks through its node's ranking.
  std::optional<moxygen::TrackFilter> trackFilter;
  std::string peerID; // relay ID of a peer relay subscriber
};

// Prefix tree of the subscriptions that want PUBLISH for every track under a
// namespace: SUBSCRIBE_TRACKS, and the track half of a SUBSCRIBE_NAMESPACE
// before draft 18. A node holds the rankings of its top-N subscribers, so a
// ranking lives exactly as long as a subscriber at that node.
class TracksTree {
public:
  using SubscriberList = std::vector<std::pair<Participant, TrackSubscriberInfo>>;

  struct Node {
  private:
    friend class TracksTree;
    explicit Node(Node* parent) : parent_(parent) {}

    struct Entry {
      Participant participant;
      TrackSubscriberInfo info;
    };
    using Key = std::pair<moxygen::SessionId, TrackSubscriptionSource>;
    struct KeyHash {
      size_t operator()(const Key& key) const {
        return folly::hash::hash_combine(
            moxygen::SessionId::hash{}(key.first),
            folly::to_underlying(key.second)
        );
      }
    };
    Node* parent_;
    folly::F14FastMap<std::string, std::unique_ptr<Node>> children_;
    folly::F14FastMap<Key, Entry, KeyHash> subscribers_;
    folly::F14FastMap<uint64_t, std::shared_ptr<PropertyRanking>> rankings_;
  };

  // The subscriber's node, whose rankings stay valid until the node's last
  // subscriber leaves. Null if prefix, session and source are already present.
  Node* addSubscriber(
      const moxygen::TrackNamespace& prefix,
      Participant subscriber,
      TrackSubscriberInfo info
  );

  // Also drops a top-N subscriber from its ranking. False if not subscribed.
  bool removeSubscriber(
      const moxygen::TrackNamespace& prefix,
      moxygen::SessionId session,
      TrackSubscriptionSource source
  );

  // Subscribers at ns and every ancestor of it.
  SubscriberList subscribersCovering(const moxygen::TrackNamespace& ns) const;

  // Calls fn(propertyType, ranking) for every ranking at ns and its ancestors.
  template <typename Fn>
  void forEachRankingOnPath(const moxygen::TrackNamespace& ns, Fn&& fn) const {
    const Node* node = &root_;
    for (size_t i = 0;; i++) {
      for (const auto& [propertyType, ranking] : node->rankings_) {
        if (ranking) {
          fn(propertyType, ranking);
        }
      }
      if (i == ns.size()) {
        return;
      }
      auto it = node->children_.find(ns[i]);
      if (it == node->children_.end()) {
        return;
      }
      node = it->second.get();
    }
  }

  // The ranking slot for propertyType at node, null until the caller fills it.
  std::shared_ptr<PropertyRanking>& ranking(Node& node, uint64_t propertyType) {
    return node.rankings_[propertyType];
  }

  // True if session has a SUBSCRIBE_TRACKS at prefix, an ancestor or a descendant.
  bool hasOverlap(const moxygen::TrackNamespace& prefix, moxygen::SessionId session) const;

private:
  Node* find(const moxygen::TrackNamespace& prefix) const;
  static bool subtreeHas(const Node& node, const Node::Key& key);
  void prune(Node* node);

  Node root_{nullptr};
};

} // namespace openmoq::moqx
