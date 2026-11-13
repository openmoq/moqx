/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "peering/MoqPeeringParticipant.h"
#include "peering/MoqPeeringRuntime.h"
#include "peering/PeeringAnnouncer.h"
#include "peering/PeeringBackend.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace openmoq::moqx {

class MoqxRelay;

// host:port, with an IPv6 host in brackets. The form executePeerAdminOp parses.
std::string moqPeerToken(std::string_view host, uint16_t port);

// One moq-peering runtime for the process. Each service relay gets one
// MoqPeeringParticipant. Peer announcements, subscribes, and objects are
// delivered to those participants. moq-peering does not open a MoQT session,
// so this is not a MoqtRelayBackend.
class MoqPeeringCore : public PeeringAnnouncer,
                       public peering::PeeringEvents,
                       public std::enable_shared_from_this<MoqPeeringCore> {
public:
  explicit MoqPeeringCore(std::shared_ptr<peering::MoqPeeringRuntime> runtime);
  ~MoqPeeringCore() override;

  MoqPeeringCore(const MoqPeeringCore&) = delete;
  MoqPeeringCore& operator=(const MoqPeeringCore&) = delete;

  void addRelay(const std::shared_ptr<MoqxRelay>& relay);
  // Accepts peer callbacks. Relays must already be added.
  void arm();
  void detach(MoqxRelay& relay);
  // Drops every relay and clears the runtime callback. Idempotent.
  void disarm();

  std::optional<peering::PeeringStatus> status() const;
  PeerAdminResult execute(PeerAdminOp op);
  std::vector<PeerLink> links() const;

  void announceNamespace(const moxygen::TrackNamespace& ns) override;
  void withdrawNamespace(const moxygen::TrackNamespace& ns) override;
  void announceTrack(const moxygen::FullTrackName& ftn) override;
  void withdrawTrack(const moxygen::FullTrackName& ftn) override;
  void sendObject(const moxygen::FullTrackName& ftn, PeeringObject object) override;
  void endSubgroup(
      const moxygen::FullTrackName& ftn,
      uint64_t group,
      uint64_t subgroup,
      bool reset
  ) override;
  void subscribeTrack(const moxygen::FullTrackName& ftn) override;
  void unsubscribeTrack(const moxygen::FullTrackName& ftn) override;
  moxygen::SessionId sessionId() const override { return sessionId_; }

  void onNamespace(std::vector<std::string> nameSpace, bool withdrawn) override;
  void onTrack(std::vector<std::string> nameSpace, std::string track, bool withdrawn) override;
  void onSubscribe(
      std::vector<std::string> nameSpace,
      std::string track,
      uint64_t trackHash,
      uint8_t priority,
      uint8_t forward
  ) override;
  void onUnsubscribe(uint64_t trackHash, bool subscribersRemain) override;
  void onData(
      uint64_t trackHash,
      bool isNewStream,
      std::optional<uint64_t> streamId,
      std::vector<uint8_t> bytes
  ) override;
  void onStreamClosed(uint64_t trackHash, uint64_t streamId, bool reset) override;

private:
  void dispatch(std::function<void(MoqPeeringParticipant&)> fn);

  std::weak_ptr<peering::MoqPeeringRuntime> runtime_;
  moxygen::SessionId sessionId_{makeSessionId()};
  std::mutex mu_;
  std::vector<std::shared_ptr<MoqPeeringParticipant>> participants_;
  std::map<uint64_t, moxygen::FullTrackName> peerSubs_;
  std::map<uint64_t, moxygen::FullTrackName> pulls_;
  struct StreamKey {
    uint64_t hash{0};
    uint64_t group{0};
    uint64_t subgroup{0};
    bool operator<(const StreamKey& other) const {
      return std::tie(hash, group, subgroup) < std::tie(other.hash, other.group, other.subgroup);
    }
  };
  std::map<StreamKey, std::unique_ptr<PeeringSubgroupWriter>> writers_;
  std::map<std::pair<uint64_t, uint64_t>, std::unique_ptr<PeeringStreamReader>> readers_;
  bool armed_{false};
};

// The PeeringBackend installed for each service when the process selected moq-peering.
// The services share one MoqPeeringCore.
class MoqPeeringBackend : public PeeringBackend {
public:
  MoqPeeringBackend(std::shared_ptr<MoqxRelay> relay, std::shared_ptr<MoqPeeringCore> core);

  void start(folly::EventBase* evb) override;
  void stop() override;

  // Peer announcements are injected when they arrive. A subscribe does not wait
  // for the QUIC handshake, so this stays false.
  bool peersPending() const override { return false; }
  folly::coro::Task<void> waitForPeers(folly::Function<bool()> routed) override;
  bool holdsSessionHandles() const override { return false; }
  ParticipantRouting routingFor(const ParticipantInfo& info) const override;

  folly::coro::Task<void> visitPeers(PeerVisitor& visitor) const override;
  folly::coro::Task<PeerAdminResult> executePeerAdminOp(PeerAdminOp op) override;

private:
  std::weak_ptr<MoqxRelay> relay_;
  std::shared_ptr<MoqPeeringCore> core_;
};

} // namespace openmoq::moqx
