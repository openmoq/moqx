/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "relay/Participant.h"

#include <folly/Executor.h>
#include <folly/Function.h>
#include <folly/coro/Task.h>
#include <folly/io/async/EventBase.h>
#include <moxygen/MoQTypes.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace openmoq::moqx {

// An admin request for one peer.
struct PeerAdminOp {
  enum class Kind { Add, Remove, SetMetric, Restart };
  Kind kind{Kind::Add};
  // Names the peer in the backend's own form. MoqtRelayBackend takes an upstream URL.
  std::string peer;
  // The new session metric, for SetMetric. Add may also set it.
  std::optional<uint64_t> metric;
};

// The outcome of a PeerAdminOp.
struct PeerAdminResult {
  enum class Error { None, NotFound, Duplicate, Invalid, Unsupported };
  // None on success. The admin handler maps the rest to HTTP 404, 409, 400 and 501.
  Error error{Error::None};
  // The peer as the backend normalized it.
  std::string peer;
  // The metric in effect after the op, when the op carried one.
  std::optional<uint64_t> metric;
  // Detail for a human, such as why an op failed, or that a new metric applies on reconnect.
  std::string message;
};

// Measurements of one peer link, for backends that take them.
struct PeerMetrics {
  uint64_t nodeId{0};
  uint64_t srttUs{0};
  uint64_t metric{0};
  uint64_t nodeMetric{0};
  uint64_t connectTimeUs{0};
  uint64_t subscribeTracks{0};
  uint64_t publishes{0};
  uint64_t activeStreams{0};
};

// One link to a peer relay, for /state and metrics.
struct PeerLink {
  enum class Direction { Dialed, Accepted };
  enum class State { Connecting, Connected, Disconnected };
  Direction direction{Direction::Dialed};
  State state{State::Connecting};
  std::string address;                // upstream URL when dialed, remote address when accepted
  std::string authority;              // accepted links only
  std::string relayID;                // when the peer presented one
  std::optional<PeerMetrics> metrics; // set when the backend measures links
};

class PeerVisitor {
public:
  virtual ~PeerVisitor() = default;
  virtual void onPeer(const PeerLink& link) = 0;
};

// Sends a reciprocal SUBSCRIBE_NAMESPACE to each inbound peer relay that presents a relay
// token, and records which sessions belong to peer relays. Only the MoQT relay backend has
// one.
class ReciprocalMoqtPeerHandler {
public:
  virtual ~ReciprocalMoqtPeerHandler() = default;

  // Returns the peer's relay ID when params carry a relay token, and unset otherwise.
  // Unset for every request when this relay does not have a relay ID. Pure, so any
  // executor may call it.
  virtual std::optional<std::string> authorizePeer(const moxygen::TrackRequestParameters& params
  ) const = 0;

  // The relay calls this on its executor when an authorized peer sends SUBSCRIBE_NAMESPACE for
  // the empty prefix. Returns false, sending nothing, while the session already has one open.
  // Otherwise records the session as the peer's, and sends the peer a SUBSCRIBE_NAMESPACE, plus
  // a SUBSCRIBE_TRACKS on draft 18 and later. Completes after both replies arrive.
  virtual folly::coro::Task<bool> onPeerConnected(Participant peer, std::string peerID) = 0;

  // The relay calls this on its executor when that SUBSCRIBE_NAMESPACE ends. Releases what
  // onPeerConnected() sent. The session stays the peer's until it closes.
  virtual void onPeerDisconnected(moxygen::SessionId session) = 0;

  // The relay calls this on its executor when a peer's session closes. Forgets the
  // session's relay ID, and releases anything that onPeerDisconnected() did not.
  virtual void onPeerClosed(moxygen::SessionId session) = 0;

  // On the relay executor. Returns the relay ID of the peer relay that owns session, or
  // unset when session is not a peer relay's.
  virtual std::optional<std::string> peerIdOf(moxygen::SessionId session) const = 0;
};

// How one service's relay reaches other relays. The context owns one per service, and
// destroys it after the relay. Every lifecycle call runs on the context's thread: start(),
// then stop(), then a drain of every executor, then join().
class PeeringBackend {
public:
  virtual ~PeeringBackend() = default;

  // Connects to the configured peers, before the servers start accepting. The backend may
  // use evb for anything that needs an EventBase, such as MoQ sessions, HTTP or QUIC.
  virtual void start(folly::EventBase* evb) = 0;

  // Withdraws the backend's routes, ends its subscriptions, and releases its handles in
  // relay state. Every executor still runs, so each release can hop to its own executor.
  virtual void stop() = 0;

  // Returns the executors that the backend runs work on, other than the IO and relay
  // executors. The shutdown drain includes them.
  virtual std::vector<folly::Executor*> executors() const { return {}; }

  // Stops the backend's own threads. Runs after the drain.
  virtual void join() {}

  // Returns info.routing with the backend's fields chosen, given the SETUP facts that it
  // already holds. Pure, so any executor may call it. The session adapter calls it when it
  // builds a session's Participant.
  virtual ParticipantRouting routingFor(const ParticipantInfo& info) const = 0;

  // On the relay executor. Returns true while a configured peer is still connecting or
  // reconnecting, so waitForPeers() could still find a route.
  virtual bool peersPending() const = 0;

  // On the relay executor, after a request misses. Completes when routed() returns true,
  // or once every peer has connected or given up. routed() reads relay state, so the
  // backend calls it on the relay executor.
  virtual folly::coro::Task<void> waitForPeers(folly::Function<bool()> routed) = 0;

  // On the relay executor. Returns true while the backend holds a handle that keeps a
  // session alive, such as a peering subscription. Debug builds check that this returns
  // false after stop().
  virtual bool holdsSessionHandles() const = 0;

  // Returns the backend's reciprocal peer handler, or null. The result does not change.
  virtual ReciprocalMoqtPeerHandler* reciprocalMoqtPeerHandler() { return nullptr; }

  // Reports each peer link, for /state and metrics. Any executor may await it. The task
  // reads each piece of state on that state's executor, and calls visitor on the awaiting
  // executor.
  virtual folly::coro::Task<void> visitPeers(PeerVisitor& visitor) const = 0;

  // Runs an admin op from the admin API. Any executor may await it. Ops that the backend
  // does not support fail with Error::Unsupported.
  virtual folly::coro::Task<PeerAdminResult> executePeerAdminOp(PeerAdminOp op) = 0;
};

} // namespace openmoq::moqx
