/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "UpstreamProvider.h"
#include "config/Config.h"
#include "peering/PeeringBackend.h"
#include "relay/ExecBound.h"
#include "relay/Participant.h"

#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <moxygen/events/MoQFollyExecutorImpl.h>

#include <memory>
#include <optional>
#include <vector>

namespace openmoq::moqx {

class MoqxRelay;

// Relay peering over MoQT sessions: dials the service's configured upstream relays, and
// reciprocates a peer relay's SUBSCRIBE_NAMESPACE.
class MoqtRelayBackend : public PeeringBackend, public ReciprocalMoqtPeerHandler {
public:
  MoqtRelayBackend(
      std::shared_ptr<MoqxRelay> relay,
      const config::ServiceConfig& service,
      std::optional<uint64_t> hopID
  );
  ~MoqtRelayBackend() override;

  void start(folly::EventBase* workerEvb) override;
  void stop() override;

  bool peersPending() const override { return connectedUpstreams_.size() < upstreams_.size(); }
  folly::coro::Task<void> waitForPeers(folly::Function<bool()> routed) override;
  bool holdsSessionHandles() const override;
  ReciprocalMoqtPeerHandler* reciprocalMoqtPeerHandler() override { return this; }

  folly::coro::Task<void> visitPeers(PeerVisitor& visitor) const override;
  folly::coro::Task<PeerAdminResult> executePeerAdminOp(PeerAdminOp op) override;

  std::optional<std::string> authorizePeer(const moxygen::TrackRequestParameters& params
  ) const override;
  folly::coro::Task<bool> onPeerConnected(Participant peer, std::string peerID) override;
  void onPeerDisconnected(moxygen::SessionId session) override;
  void onPeerClosed(moxygen::SessionId session) override;
  std::optional<std::string> peerIdOf(moxygen::SessionId session) const override;

  // Called by UpstreamProvider's onConnect hook after a new upstream session is
  // established. Issues the peer subNs handshake and saves the handle.
  folly::coro::Task<void> onUpstreamConnect(Participant upstream);

  // Called by UpstreamProvider's onDisconnect hook when an upstream session
  // closes. Releases that session's peering handles.
  void onUpstreamDisconnect(moxygen::SessionId upstream);

  // For tests that build their own provider. Call before the relay serves requests.
  void addUpstream(std::shared_ptr<UpstreamProvider> upstream) {
    upstreams_.push_back(std::move(upstream));
  }

private:
  void startUpstream(
      const config::UpstreamConfig& cfg,
      const std::shared_ptr<moxygen::MoQFollyExecutorImpl>& workerExec
  );

  // The relay points back at this backend, so the relay is destroyed first. stop() makes the
  // providers' filters drop their references to the relay.
  std::weak_ptr<MoqxRelay> relay_;
  folly::Executor* relayExec_;
  std::string relayID_;
  std::vector<config::UpstreamConfig> configs_;
  std::optional<uint64_t> hopID_;
  uint64_t subscriptionBufferBytes_;
  folly::EventBase* workerEvb_{nullptr};
  // Filled by start(), before the servers accept requests.
  std::vector<std::shared_ptr<UpstreamProvider>> upstreams_;
  // Upstream sessions whose handshake has finished. On the relay executor.
  folly::F14FastSet<moxygen::SessionId, moxygen::SessionId::hash> connectedUpstreams_;
  bool stopped_{false};

  struct UpstreamHandles {
    ExecBound<std::shared_ptr<moxygen::Publisher::SubscribeNamespaceHandle>> subNs;
    ExecBound<std::shared_ptr<moxygen::Publisher::SubscribeTracksHandle>> tracks; // draft 18+
  };
  // Keeps each connected upstream session's peering subscriptions alive. On the relay executor.
  folly::F14FastMap<moxygen::SessionId, UpstreamHandles, moxygen::SessionId::hash> upstreamHandles_;

  struct PeerInfo {
    std::string relayID;
    std::string address;
    std::string authority;
    // The reciprocal subscriptions, until the peer's SUBSCRIBE_NAMESPACE ends.
    ExecBound<std::shared_ptr<moxygen::Publisher::SubscribeNamespaceHandle>> subNs;
    ExecBound<std::shared_ptr<moxygen::Publisher::SubscribeTracksHandle>> tracks; // draft 18+
    bool open{false}; // while the peer's SUBSCRIBE_NAMESPACE is open
  };
  // Each peer relay session, from onPeerConnected() until it closes. On the relay executor.
  folly::F14FastMap<moxygen::SessionId, PeerInfo, moxygen::SessionId::hash> peers_;
};

} // namespace openmoq::moqx
