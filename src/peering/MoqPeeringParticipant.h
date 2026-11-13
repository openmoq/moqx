/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "peering/PeeringAnnouncer.h"
#include "relay/Participant.h"

#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include <moxygen/MoQConsumers.h>
#include <moxygen/MoQTypes.h>

namespace openmoq::moqx {

class MoqxRelay;

// One participant for every peer. moq-peering collapses peers into one subscribe
// per track, so the relay sees a single publisher and a single subscriber.
// Peer announces are published into the relay. Local publishes are announced
// back out. Objects use the track consumers the relay already gives a client.
//
// announcer is kept alive until in-flight relay callbacks finish. Callbacks that
// touch the relay run on the relay executor.
class MoqPeeringParticipant : public moxygen::Publisher,
                              public moxygen::Subscriber,
                              public moxygen::Publisher::NamespacePublishHandle,
                              public std::enable_shared_from_this<MoqPeeringParticipant> {
public:
  MoqPeeringParticipant(
      std::shared_ptr<PeeringAnnouncer> announcer,
      std::shared_ptr<MoqxRelay> relay
  );

  // Subscribes to the whole namespace and to every track, with forward=false.
  // A later peer subscribe turns forwarding on for that track only.
  void attach();
  void detach();
  bool ready() const { return ready_.load(std::memory_order_acquire); }

  std::shared_ptr<MoqxRelay> relay() const { return relay_.lock(); }

  void onNamespace(moxygen::TrackNamespace ns, bool withdrawn);
  void onTrack(moxygen::FullTrackName ftn, bool withdrawn);
  void onSubscribe(const moxygen::FullTrackName& ftn);
  void onUnsubscribe(const moxygen::FullTrackName& ftn);
  void deliverObject(const moxygen::FullTrackName& ftn, const PeeringObject& object);
  void acceptEvent(
      const moxygen::FullTrackName& ftn,
      uint64_t streamId,
      const PeeringStreamReader::Event& event
  );
  void finishIngress(const moxygen::FullTrackName& ftn, uint64_t streamId, bool reset);

  void namespaceMsg(const moxygen::TrackNamespace& trackNamespaceSuffix) override;
  void namespaceDoneMsg(const moxygen::TrackNamespace& trackNamespaceSuffix) override;

  folly::coro::Task<SubscribeResult> subscribe(
      moxygen::SubscribeRequest sub,
      std::shared_ptr<moxygen::TrackConsumer> callback
  ) override;

  PublishResult publish(
      moxygen::PublishRequest pub,
      std::shared_ptr<SubscriptionHandle> handle
  ) override;

private:
  struct PeerNamespace {
    bool cancel{false};
    std::shared_ptr<PublishNamespaceHandle> handle;
  };

  void prepare();
  folly::coro::Task<void> attachOnRelay();
  folly::coro::Task<void> publishPeerNamespace(moxygen::TrackNamespace ns);
  folly::coro::Task<void> publishPeerTrack(moxygen::FullTrackName ftn);
  void withdrawPeerNamespace(const moxygen::TrackNamespace& ns);
  void endPeerTrack(const moxygen::FullTrackName& ftn);

  void pull(const moxygen::FullTrackName& ftn);
  void unpull(const moxygen::FullTrackName& ftn);
  void setEgressForward(const moxygen::FullTrackName& ftn, bool forward);
  void onLocalTrackDone(const moxygen::FullTrackName& ftn);
  void sendLocalObject(const moxygen::FullTrackName& ftn, PeeringObject object);
  void endLocalSubgroup(
      const moxygen::FullTrackName& ftn,
      uint64_t group,
      uint64_t subgroup,
      bool reset
  );
  void announceNamespaceOnce(const moxygen::TrackNamespace& ns, bool fromSubscription);
  void withdrawAnnouncedNamespace(const moxygen::TrackNamespace& ns);

  template <typename T> void spawn(folly::coro::Task<T> task);

  std::vector<std::shared_ptr<moxygen::TrackConsumer>>
  ingressTargets(const moxygen::FullTrackName& ftn) const;
  void writeObject(const moxygen::FullTrackName& ftn, const PeeringObject& object);

  Participant asParticipant() const;

  PeeringAnnouncer* announcer() const { return announcer_.get(); }

  std::shared_ptr<PeeringAnnouncer> announcer_;
  std::weak_ptr<MoqxRelay> relay_;
  std::shared_ptr<const ParticipantInfo> info_;
  folly::Executor* executor_{nullptr};
  std::atomic<bool> ready_{false};
  bool attached_{false};
  uint64_t nextRequest_{1};

  std::shared_ptr<SubscribeNamespaceHandle> namespaceSub_;
  std::shared_ptr<SubscribeTracksHandle> tracksSub_;

  std::map<moxygen::TrackNamespace, PeerNamespace> peerNamespaces_;
  std::map<moxygen::FullTrackName, std::shared_ptr<moxygen::TrackConsumer>> ingress_;
  std::map<moxygen::FullTrackName, std::vector<std::shared_ptr<moxygen::TrackConsumer>>>
      pullConsumers_;
  std::map<
      std::pair<moxygen::FullTrackName, uint64_t>,
      std::vector<std::shared_ptr<moxygen::SubgroupConsumer>>>
      ingressStreams_;
  std::set<moxygen::FullTrackName> originated_;
  std::set<moxygen::FullTrackName> pulled_;

  std::map<moxygen::FullTrackName, std::shared_ptr<SubscriptionHandle>> egress_;
  // Last forward flag a peer subscribe asked for. PUBLISH_OK is filled from this
  // when the relay reads it, so a subscribe that arrived first is not cleared
  // by a later forward=false ok.
  std::map<moxygen::FullTrackName, bool> egressForward_;
  std::set<moxygen::FullTrackName> announcedTracks_;
  struct AnnouncedNamespace {
    bool announced{false};
    bool subscription{false};
  };
  std::map<moxygen::TrackNamespace, AnnouncedNamespace> announcedNamespaces_;

  friend class PeeringIngressHandle;
  friend class PeeringPullHandle;
  friend class PeeringEgressTrack;
  friend class PeeringEgressSubgroup;
};

} // namespace openmoq::moqx
