/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "UpstreamProvider.h"
#include "config/Config.h"
#include "peering/PeeringBackend.h"
#include "relay/Participant.h"

#include <folly/container/F14Set.h>
#include <moxygen/events/MoQFollyExecutorImpl.h>

#include <memory>
#include <optional>
#include <vector>

namespace openmoq::moqx {

class MoqxRelay;

// Relay peering over MoQT sessions: dials the service's configured upstream relays.
class MoqtRelayBackend : public PeeringBackend {
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
  void visitState(RelayStateVisitor& /*visitor*/) const override {}
  bool holdsSessionHandles() const override { return false; }

  folly::coro::Task<void> visitPeers(PeerVisitor& visitor) const override;
  folly::coro::Task<PeerAdminResult> executePeerAdminOp(PeerAdminOp op) override;

  // For tests that build their own provider. Call before the relay serves requests.
  void addUpstream(std::shared_ptr<UpstreamProvider> upstream) {
    upstreams_.push_back(std::move(upstream));
  }

private:
  // On the relay executor.
  folly::coro::Task<void> onUpstreamConnect(std::shared_ptr<MoqxRelay> relay, Participant upstream);
  void startUpstream(
      const config::UpstreamConfig& cfg,
      const std::shared_ptr<moxygen::MoQFollyExecutorImpl>& workerExec
  );

  // The relay points back at this backend, so the relay is destroyed first. stop() makes the
  // providers' filters drop their references to the relay.
  std::weak_ptr<MoqxRelay> relay_;
  folly::Executor* relayExec_;
  std::vector<config::UpstreamConfig> configs_;
  std::optional<uint64_t> hopID_;
  uint64_t subscriptionBufferBytes_;
  folly::EventBase* workerEvb_{nullptr};
  // Filled by start(), before the servers accept requests.
  std::vector<std::shared_ptr<UpstreamProvider>> upstreams_;
  // Upstream sessions, by whether their handshake has finished. On the relay executor.
  folly::F14FastSet<moxygen::SessionId, moxygen::SessionId::hash> handshakingUpstreams_;
  folly::F14FastSet<moxygen::SessionId, moxygen::SessionId::hash> connectedUpstreams_;
  bool stopped_{false};
};

} // namespace openmoq::moqx
