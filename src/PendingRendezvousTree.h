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

// Shared between PendingRendezvousTree and the parked subscribe coroutine's
// RendezvousTicket (control-plane only, one per rendezvous SUBSCRIBE). Also keeps
// the waiter alive in toWake while signal() resumes the coroutine inline and
// re-enters eraseWaiter().
struct RendezvousWaiter {
  explicit RendezvousWaiter(moxygen::SessionId id) : sessionId(id) {}
  moxygen::TimedBaton baton;
  moxygen::SessionId sessionId;
  bool signalled{false}; // read and written on the relay exec only
};

// Tree of SUBSCRIBEs parked on a namespace/track that isn't published yet
// (draft 18+ RENDEZVOUS_TIMEOUT), indexed by namespace prefix. Woken via
// wakeForTrack()/wakeUnderNamespace() when matching content arrives;
// otherwise each waiter's baton times out on its own. Woken waiters stay in
// the tree (marked `signalled`) until their RendezvousTicket calls eraseWaiter(),
// so isWaiting() continues to report them and grant SUBSCRIBE precedence over
// relay-sent PUBLISHes until the subscriber's forwarder slot is taken.
class PendingRendezvousTree {
public:
  // Parks `waiter` on ftn until eraseWaiter() is called.
  void addWaiter(const moxygen::FullTrackName& ftn, std::shared_ptr<RendezvousWaiter> waiter);

  // Removes `waiter` from ftn's park list, pruning now-empty ancestor nodes.
  void eraseWaiter(const moxygen::FullTrackName& ftn, const RendezvousWaiter* waiter);

  // Marks and signals unsignalled waiters for this exact track (a PUBLISH landed).
  // Waiters stay in the tree until eraseWaiter(); other tracks under the same
  // namespace node are untouched.
  void wakeForTrack(const moxygen::FullTrackName& ftn);

  // Marks and signals every unsignalled waiter under this namespace (a
  // PUBLISH_NAMESPACE landed) — any track under it may now be resolvable.
  // Waiters stay in the tree until eraseWaiter().
  void wakeUnderNamespace(const moxygen::TrackNamespace& ns);

  // True if no waiter is in the tree. Mainly for tests to confirm
  // eraseWaiter's ancestor pruning leaves no dangling nodes behind.
  bool empty() const { return root_.empty(); }

  // True if `sessionId` has a waiter in the tree for `ftn` (signalled or not).
  bool isWaiting(const moxygen::FullTrackName& ftn, moxygen::SessionId sessionId) const;

private:
  struct Node {
    using WaiterList = std::vector<std::shared_ptr<RendezvousWaiter>>;

    bool empty() const { return children.empty() && waitersByTrack.empty(); }

    folly::F14FastMap<std::string, std::unique_ptr<Node>> children;
    folly::F14FastMap<std::string, WaiterList> waitersByTrack;
  };

  Node& findOrCreateNode(const moxygen::TrackNamespace& ns);

  // Marks every unsignalled waiter in this subtree as signalled and appends it
  // to `out` without removing it from the tree. Don't signal them until the
  // walk is complete: signal() can resume a waiter synchronously, and it would
  // re-enter eraseWaiter() mid-walk.
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
