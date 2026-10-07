/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PendingRendezvousTree.h"

#include <moxygen/MoQSession.h>
#include <moxygen/MoQVersions.h>

#include <folly/logging/xlog.h>

#include <algorithm>
#include <string_view>

using namespace moxygen;

namespace openmoq::moqx {

PendingRendezvousTree::Node& PendingRendezvousTree::findOrCreateNode(const TrackNamespace& ns) {
  auto* node = &root_;
  for (const auto& part : ns.trackNamespace) {
    auto& child = node->children[part];
    if (!child) {
      child = std::make_unique<Node>();
    }
    node = child.get();
  }
  return *node;
}

void PendingRendezvousTree::addWaiter(
    const FullTrackName& ftn,
    const std::shared_ptr<moxygen::TimedBaton>& waiter
) {
  auto& node = findOrCreateNode(ftn.trackNamespace);
  node.waitersByTrack[ftn.trackName].push_back(waiter);
}

void PendingRendezvousTree::eraseWaiter(
    const FullTrackName& ftn,
    const std::shared_ptr<moxygen::TimedBaton>& waiter
) {
  applyAtNamespaceNode(ftn.trackNamespace, [&](Node& node) {
    auto waitersIt = node.waitersByTrack.find(ftn.trackName);
    if (waitersIt == node.waitersByTrack.end()) {
      return;
    }
    auto& waiters = waitersIt->second;
    waiters.erase(std::remove(waiters.begin(), waiters.end(), waiter), waiters.end());
    if (waiters.empty()) {
      node.waitersByTrack.erase(waitersIt);
    }
  });
}

// Removes every waiter in this subtree from the tree (namespace-level
// publish/publishNamespace) into `out`, since all of it is now resolved.
// Does NOT signal: see collectSubtree's declaration comment.
void PendingRendezvousTree::collectSubtree(Node& node, Node::WaiterList& out) {
  for (auto& [_, trackWaiters] : node.waitersByTrack) {
    out.insert(
        out.end(),
        std::make_move_iterator(trackWaiters.begin()),
        std::make_move_iterator(trackWaiters.end())
    );
  }
  node.waitersByTrack.clear();
  for (auto& [_, child] : node.children) {
    collectSubtree(*child, out);
  }
  node.children.clear();
}

void PendingRendezvousTree::applyAtNamespaceNode(
    const TrackNamespace& ns,
    folly::FunctionRef<void(Node&)> onNode
) {
  auto* node = &root_;
  std::vector<std::pair<Node*, std::string_view>> path;
  bool foundNamespace = true;
  for (const auto& part : ns.trackNamespace) {
    auto childIt = node->children.find(part);
    if (childIt == node->children.end()) {
      foundNamespace = false;
      break;
    }
    path.emplace_back(node, part);
    node = childIt->second.get();
  }

  if (foundNamespace) {
    onNode(*node);
  }
  // defensive clean up after waking
  for (auto it = path.rbegin(); it != path.rend() && node->empty(); ++it) {
    auto* parent = it->first;
    parent->children.erase(it->second);
    node = parent;
  }
}

void PendingRendezvousTree::wakeForTrack(const FullTrackName& ftn) {
  Node::WaiterList toWake;
  applyAtNamespaceNode(ftn.trackNamespace, [&](Node& node) {
    auto waitersIt = node.waitersByTrack.find(ftn.trackName);
    if (waitersIt != node.waitersByTrack.end()) {
      toWake = std::move(waitersIt->second);
      node.waitersByTrack.erase(waitersIt);
    }
  });
  // Signal only once the tree walk/prune above is fully done: TimedBaton::signal()
  // may resume the parked coroutine synchronously, re-entering eraseWaiter()
  // which must not observe (or corrupt) a container we were still iterating/mutating.
  for (auto& waiter : toWake) {
    waiter->signal();
  }
}

void PendingRendezvousTree::wakeUnderNamespace(const TrackNamespace& ns) {
  Node::WaiterList toWake;
  applyAtNamespaceNode(ns, [&](Node& node) { collectSubtree(node, toWake); });
  // See the comment in wakeForTrack: signal strictly after all tree mutation completes.
  for (auto& waiter : toWake) {
    waiter->signal();
  }
}

std::optional<std::chrono::milliseconds>
takeRendezvousTimeout(SubscribeRequest& subReq, const std::shared_ptr<MoQSession>& session) {
  // Applicable for d18+ only
  auto version = session ? session->getNegotiatedVersion() : std::optional<uint64_t>{};
  if (!version.has_value() || getDraftMajorVersion(*version) < 18) {
    return std::nullopt;
  }
  auto ms = getFirstIntParam(subReq.params, TrackRequestParamKey::RENDEZVOUS_TIMEOUT);
  // Per-hop param: consume it so it can never reach the upstream request.
  subReq.params.eraseAllParamsOfType(TrackRequestParamKey::RENDEZVOUS_TIMEOUT);
  if (!ms.has_value() || *ms == 0) {
    return std::nullopt;
  }

  // Hardcoded ceiling: bounds how long a rendezvous SUBSCRIBE
  // can park a waiter, regardless of the client-requested RENDEZVOUS_TIMEOUT.
  constexpr auto kMaxMs = static_cast<uint64_t>(kMaxRendezvousTimeout.count());
  if (*ms > kMaxMs) {
    XLOG(DBG1) << "Clamping RENDEZVOUS_TIMEOUT for " << subReq.fullTrackName << " from " << *ms
               << "ms to " << kMaxMs << "ms";
  }
  return std::chrono::milliseconds(std::min(*ms, kMaxMs));
}

} // namespace openmoq::moqx
