/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/MoQTypes.h>
#include <moxygen/util/TimedBaton.h>

#include <folly/Function.h>
#include <folly/container/F14Map.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace moxygen {
class MoQSession;
} // namespace moxygen

namespace openmoq::moqx {

// Tree of SUBSCRIBEs parked on a namespace/track that isn't published yet
// (draft 18+ RENDEZVOUS_TIMEOUT), indexed by namespace prefix. Woken via
// wakeForTrack()/wakeUnderNamespace() when matching content arrives;
// otherwise each waiter's baton times out on its own.
class PendingRendezvousTree {
public:
  // Parks `waiter` on ftn until erased or woken.
  void
  addWaiter(const moxygen::FullTrackName& ftn, const std::shared_ptr<moxygen::TimedBaton>& waiter);

  // Removes `waiter` from ftn's park list, pruning now-empty ancestor nodes.
  void eraseWaiter(
      const moxygen::FullTrackName& ftn,
      const std::shared_ptr<moxygen::TimedBaton>& waiter
  );

  // Wakes only waiters for this exact track (a PUBLISH landed). Waiters for
  // other tracks under the same namespace node are left parked.
  void wakeForTrack(const moxygen::FullTrackName& ftn);

  // Wakes every waiter under this namespace (a PUBLISH_NAMESPACE landed) — any
  // track under it may now be resolvable.
  void wakeUnderNamespace(const moxygen::TrackNamespace& ns);

  // True if no waiter is parked anywhere in the tree. Mainly for tests to
  // confirm eraseWaiter's ancestor pruning leaves no dangling nodes behind.
  bool empty() const { return root_.empty(); }

private:
  struct Node {
    using WaiterList = std::vector<std::shared_ptr<moxygen::TimedBaton>>;

    bool empty() const { return children.empty() && waitersByTrack.empty(); }

    folly::F14FastMap<std::string, std::unique_ptr<Node>> children;
    folly::F14FastMap<std::string, WaiterList> waitersByTrack;
  };

  Node& findOrCreateNode(const moxygen::TrackNamespace& ns);
  // Removes every waiter in this subtree from the tree and appends it to `out`.
  // Don't signal them until the caller is done mutating the tree: signal() can
  // resume a waiter synchronously, and it would re-enter eraseWaiter() mid-walk.
  static void collectSubtree(Node& node, Node::WaiterList& out);
  // Walks down to the node at `ns`, applies `onNode`, then prunes any
  // now-empty ancestors on the way back up.
  void
  applyAtNamespaceNode(const moxygen::TrackNamespace& ns, folly::FunctionRef<void(Node&)> onNode);

  Node root_;
};

// Hardcoded ceiling: bounds how long a rendezvous SUBSCRIBE can park a waiter,
// regardless of the client-requested RENDEZVOUS_TIMEOUT.
inline constexpr std::chrono::milliseconds kMaxRendezvousTimeout{30'000};

// Clamped timeout if this SUBSCRIBE asks for a rendezvous, else nullopt; consumes the
// param regardless of clamping. Reads nothing but subReq/session, so callers may
// screen on any exec before hopping.
std::optional<std::chrono::milliseconds> takeRendezvousTimeout(
    moxygen::SubscribeRequest& subReq,
    const std::shared_ptr<moxygen::MoQSession>& session
);

} // namespace openmoq::moqx
