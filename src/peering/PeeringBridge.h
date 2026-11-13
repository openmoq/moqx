/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "peering/MoqPeeringRuntime.h"
#include "peering/PeeringAnnouncer.h"
#include "relay/Participant.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace openmoq::moqx {

class MoqxRelay;

// Connects every service relay to one moq-peering runtime.
//
// A local PUBLISH_NAMESPACE becomes a namespace announcement, and a local
// PUBLISH becomes a track announcement. A peer's announcement is injected
// into each relay, which is what SUBSCRIBE_NAMESPACE and SUBSCRIBE_TRACKS
// already deliver to clients. Objects use the same split: a peer subscribe
// pulls local objects out, and a local subscribe pulls peer objects in.
class PeeringBridge : public PeeringAnnouncer,
                      public peering::PeeringEvents,
                      public std::enable_shared_from_this<PeeringBridge> {
public:
  explicit PeeringBridge(std::shared_ptr<peering::MoqPeeringRuntime> runtime);
  ~PeeringBridge() override;

  PeeringBridge(const PeeringBridge&) = delete;
  PeeringBridge& operator=(const PeeringBridge&) = delete;

  void attach(const std::vector<std::shared_ptr<MoqxRelay>>& relays);

  // Stops callbacks and drops each relay's announcer. The runtime may still
  // be running; destroy it after this returns so in-flight callbacks finish
  // against a live bridge.
  void detach();

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
  void dispatch(std::function<void(MoqxRelay&)> fn);

  std::weak_ptr<peering::MoqPeeringRuntime> runtime_;
  moxygen::SessionId sessionId_{makeSessionId()};
  std::mutex mu_;
  std::vector<std::weak_ptr<MoqxRelay>> relays_;
  // Hash to name for tracks peers subscribe to, and tracks we subscribe to.
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
  bool detached_{false};
};

} // namespace openmoq::moqx
