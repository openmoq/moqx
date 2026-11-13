/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "peering/MoqPeeringParticipant.h"

#include "MoqxRelay.h"
#include "SafeTrackName.h"
#include "relay/PlainRouteCodec.h"

#include <folly/Executor.h>
#include <folly/io/IOBuf.h>
#include <folly/logging/xlog.h>
#include <moxygen/MoQConsumers.h>
#include <moxygen/MoQVersions.h>

namespace openmoq::moqx {
namespace {

// Runs a queued function on the calling thread. Single-thread relays have no
// event base; coro::Task refuses InlineExecutor, and this is not one.
class CallingThreadExecutor final : public folly::Executor {
public:
  void add(folly::Func func) override { func(); }
};

CallingThreadExecutor& callingThreadExecutor() {
  static CallingThreadExecutor executor;
  return executor;
}

std::optional<std::vector<uint8_t>> copyPayload(const moxygen::Payload& payload) {
  if (!payload) {
    return std::vector<uint8_t>{};
  }
  const size_t len = payload->computeChainDataLength();
  if (len > kMaxPeeringPayload) {
    return std::nullopt;
  }
  if (len == 0) {
    return std::vector<uint8_t>{};
  }
  auto copy = payload->clone();
  copy->coalesce();
  const auto* data = copy->data();
  return std::vector<uint8_t>(data, data + copy->length());
}

folly::coro::Task<void> updateEgress(
    std::shared_ptr<moxygen::SubscriptionHandle> handle,
    moxygen::RequestUpdate update
) {
  co_await handle->requestUpdate(std::move(update));
}

moxygen::ObjectHeader headerFrom(const PeeringObject& object) {
  return moxygen::ObjectHeader(
      object.group,
      object.subgroup,
      object.objectId,
      object.priority,
      static_cast<moxygen::ObjectStatus>(object.status),
      object.extensions
  );
}

} // namespace

// Returned when a local subscribe is already serving a track we are injecting.
constexpr std::string_view kTrackAlreadyServed = "moq-peering track already served";

class PeeringIngressHandle : public moxygen::SubscriptionHandle {
public:
  PeeringIngressHandle(
      std::weak_ptr<MoqPeeringParticipant> owner,
      moxygen::FullTrackName ftn,
      moxygen::RequestID requestID
  )
      : owner_(std::move(owner)), ftn_(std::move(ftn)) {
    moxygen::SubscribeOk ok;
    ok.requestID = requestID;
    setSubscribeOk(std::move(ok));
  }

  void unsubscribe() override {
    if (auto owner = owner_.lock()) {
      owner->unpull(ftn_);
    }
  }

  folly::coro::Task<RequestUpdateResult> requestUpdate(moxygen::RequestUpdate update) override {
    if (auto owner = owner_.lock(); owner && update.forward.has_value()) {
      if (*update.forward) {
        owner->pull(ftn_);
      } else {
        owner->unpull(ftn_);
      }
    }
    co_return moxygen::RequestOk{.requestID = update.requestID};
  }

private:
  std::weak_ptr<MoqPeeringParticipant> owner_;
  moxygen::FullTrackName ftn_;
};

class PeeringPullHandle : public moxygen::SubscriptionHandle {
public:
  PeeringPullHandle(
      std::weak_ptr<MoqPeeringParticipant> owner,
      moxygen::FullTrackName ftn,
      moxygen::RequestID requestID,
      bool forward
  )
      : owner_(std::move(owner)), ftn_(std::move(ftn)), pulling_(forward) {
    moxygen::SubscribeOk ok;
    ok.requestID = requestID;
    setSubscribeOk(std::move(ok));
    if (pulling_) {
      if (auto owner = owner_.lock()) {
        owner->pull(ftn_);
      }
    }
  }

  void unsubscribe() override {
    auto owner = owner_.lock();
    if (!owner) {
      return;
    }
    if (pulling_) {
      pulling_ = false;
      owner->unpull(ftn_);
    }
    auto consumers = owner->pullConsumers_.find(ftn_);
    if (consumers == owner->pullConsumers_.end()) {
      return;
    }
    if (consumer_) {
      std::erase(consumers->second, consumer_);
    }
    if (consumers->second.empty()) {
      owner->pullConsumers_.erase(consumers);
    }
  }

  folly::coro::Task<RequestUpdateResult> requestUpdate(moxygen::RequestUpdate update) override {
    if (update.forward.has_value()) {
      if (auto owner = owner_.lock()) {
        if (*update.forward && !pulling_) {
          pulling_ = true;
          owner->pull(ftn_);
        } else if (!*update.forward && pulling_) {
          pulling_ = false;
          owner->unpull(ftn_);
        }
      }
    }
    co_return moxygen::RequestOk{.requestID = update.requestID};
  }

  void setConsumer(std::shared_ptr<moxygen::TrackConsumer> consumer) { consumer_ = std::move(consumer); }

private:
  std::weak_ptr<MoqPeeringParticipant> owner_;
  moxygen::FullTrackName ftn_;
  std::shared_ptr<moxygen::TrackConsumer> consumer_;
  bool pulling_{false};
};

class PeeringEgressSubgroup : public moxygen::SubgroupConsumer {
public:
  PeeringEgressSubgroup(
      std::weak_ptr<MoqPeeringParticipant> owner,
      moxygen::FullTrackName ftn,
      uint64_t group,
      uint64_t subgroup,
      uint8_t priority
  )
      : owner_(std::move(owner)), ftn_(std::move(ftn)), group_(group), subgroup_(subgroup),
        priority_(priority) {}

  folly::Expected<folly::Unit, moxygen::MoQPublishError> object(
      uint64_t objectID,
      moxygen::Payload payload,
      moxygen::Extensions extensions = moxygen::noExtensions(),
      bool finSubgroup = false
  ) override {
    send(objectID, static_cast<uint8_t>(moxygen::ObjectStatus::NORMAL), finSubgroup, payload, extensions);
    if (finSubgroup) {
      end(false);
    }
    return folly::unit;
  }

  folly::Expected<folly::Unit, moxygen::MoQPublishError> beginObject(
      uint64_t objectID,
      uint64_t length,
      moxygen::Payload initialPayload,
      moxygen::Extensions extensions = moxygen::noExtensions()
  ) override {
    pending_ = {};
    if (length > kMaxPeeringPayload) {
      XLOG(WARNING) << "moq-peering dropped an object larger than 1MiB";
      return folly::unit;
    }
    auto copied = copyPayload(initialPayload);
    if (!copied || copied->size() > length) {
      return folly::unit;
    }
    pending_.active = true;
    pending_.objectId = objectID;
    pending_.expected = length;
    pending_.bytes = std::move(*copied);
    pending_.extensions = std::move(extensions);
    if (pending_.bytes.size() == length) {
      emit(false);
    }
    return folly::unit;
  }

  folly::Expected<moxygen::ObjectPublishStatus, moxygen::MoQPublishError>
  objectPayload(moxygen::Payload payload, bool finSubgroup = false) override {
    if (!pending_.active) {
      return moxygen::ObjectPublishStatus::DONE;
    }
    auto copied = copyPayload(payload);
    const size_t add = copied ? copied->size() : 0;
    if (!copied || pending_.bytes.size() + add > pending_.expected) {
      pending_ = {};
      return moxygen::ObjectPublishStatus::DONE;
    }
    pending_.bytes.insert(pending_.bytes.end(), copied->begin(), copied->end());
    if (pending_.bytes.size() == pending_.expected || finSubgroup) {
      emit(finSubgroup);
      if (finSubgroup) {
        end(false);
      }
      return moxygen::ObjectPublishStatus::DONE;
    }
    return moxygen::ObjectPublishStatus::IN_PROGRESS;
  }

  folly::Expected<folly::Unit, moxygen::MoQPublishError> endOfGroup(uint64_t objectID) override {
    moxygen::Payload empty;
    send(objectID, static_cast<uint8_t>(moxygen::ObjectStatus::END_OF_GROUP), true, empty, {});
    end(false);
    return folly::unit;
  }

  folly::Expected<folly::Unit, moxygen::MoQPublishError>
  endOfTrackAndGroup(uint64_t objectID) override {
    moxygen::Payload empty;
    send(objectID, static_cast<uint8_t>(moxygen::ObjectStatus::END_OF_TRACK), true, empty, {});
    end(false);
    return folly::unit;
  }

  folly::Expected<folly::Unit, moxygen::MoQPublishError> endOfSubgroup() override {
    end(false);
    return folly::unit;
  }

  void reset(moxygen::ResetStreamErrorCode) override { end(true); }

private:
  void send(
      uint64_t objectID,
      uint8_t status,
      bool lastInGroup,
      const moxygen::Payload& payload,
      const moxygen::Extensions& extensions
  ) {
    auto owner = owner_.lock();
    if (!owner || closed_) {
      return;
    }
    auto bytes = copyPayload(payload);
    if (!bytes) {
      XLOG(WARNING) << "moq-peering dropped an object larger than 1MiB";
      return;
    }
    PeeringObject object;
    object.datagram = false;
    object.lastInGroup = lastInGroup;
    object.status = status;
    object.priority = priority_;
    object.group = group_;
    object.subgroup = subgroup_;
    object.objectId = objectID;
    object.extensions = extensions;
    object.payload = std::move(*bytes);
    owner->sendLocalObject(ftn_, std::move(object));
  }

  void emit(bool lastInGroup) {
    auto payload = folly::IOBuf::copyBuffer(pending_.bytes.data(), pending_.bytes.size());
    send(
        pending_.objectId,
        static_cast<uint8_t>(moxygen::ObjectStatus::NORMAL),
        lastInGroup,
        payload,
        pending_.extensions
    );
    pending_ = {};
  }

  void end(bool resetStream) {
    if (closed_) {
      return;
    }
    closed_ = true;
    if (auto owner = owner_.lock()) {
      owner->endLocalSubgroup(ftn_, group_, subgroup_, resetStream);
    }
  }

  struct Pending {
    bool active{false};
    uint64_t objectId{0};
    uint64_t expected{0};
    std::vector<uint8_t> bytes;
    moxygen::Extensions extensions;
  };

  std::weak_ptr<MoqPeeringParticipant> owner_;
  moxygen::FullTrackName ftn_;
  uint64_t group_{0};
  uint64_t subgroup_{0};
  uint8_t priority_{0};
  bool closed_{false};
  Pending pending_;
};

class PeeringEgressTrack : public moxygen::TrackConsumer {
public:
  PeeringEgressTrack(std::weak_ptr<MoqPeeringParticipant> owner, moxygen::FullTrackName ftn)
      : owner_(std::move(owner)), ftn_(std::move(ftn)) {}

  folly::Expected<folly::Unit, moxygen::MoQPublishError>
  setTrackAlias(moxygen::TrackAlias) override {
    return folly::unit;
  }

  folly::Expected<std::shared_ptr<moxygen::SubgroupConsumer>, moxygen::MoQPublishError>
  beginSubgroup(
      uint64_t groupID,
      uint64_t subgroupID,
      moxygen::Priority priority,
      BeginSubgroupOptions = {}
  ) override {
    auto subgroup = std::make_shared<PeeringEgressSubgroup>(
        owner_,
        ftn_,
        groupID,
        subgroupID,
        static_cast<uint8_t>(priority)
    );
    return subgroup;
  }

  folly::Expected<folly::SemiFuture<folly::Unit>, moxygen::MoQPublishError>
  awaitStreamCredit() override {
    return folly::makeSemiFuture(folly::unit);
  }

  folly::Expected<folly::Unit, moxygen::MoQPublishError>
  objectStream(const moxygen::ObjectHeader& header, moxygen::Payload payload, bool lastInGroup = false)
      override {
    return send(header, std::move(payload), lastInGroup, false);
  }

  folly::Expected<folly::Unit, moxygen::MoQPublishError>
  datagram(const moxygen::ObjectHeader& header, moxygen::Payload payload, bool lastInGroup = false)
      override {
    return send(header, std::move(payload), lastInGroup, true);
  }

  folly::Expected<folly::Unit, moxygen::MoQPublishError>
  publishDone(moxygen::PublishDone) override {
    if (auto owner = owner_.lock()) {
      owner->onLocalTrackDone(ftn_);
    }
    return folly::unit;
  }

private:
  folly::Expected<folly::Unit, moxygen::MoQPublishError> send(
      const moxygen::ObjectHeader& header,
      moxygen::Payload payload,
      bool lastInGroup,
      bool datagramObject
  ) {
    auto owner = owner_.lock();
    if (!owner) {
      return folly::unit;
    }
    auto bytes = copyPayload(payload);
    if (!bytes) {
      XLOG(WARNING) << "moq-peering dropped an object larger than 1MiB";
      return folly::unit;
    }
    PeeringObject object;
    object.datagram = datagramObject;
    object.lastInGroup = lastInGroup;
    object.status = static_cast<uint8_t>(header.status);
    object.priority = header.priority.value_or(moxygen::kDefaultPriority);
    object.group = header.group;
    object.subgroup = datagramObject ? 0 : header.subgroup;
    object.objectId = header.id;
    object.extensions = header.extensions;
    object.payload = std::move(*bytes);
    owner->sendLocalObject(ftn_, std::move(object));
    if (lastInGroup && !datagramObject) {
      owner->endLocalSubgroup(ftn_, header.group, header.subgroup, false);
    }
    return folly::unit;
  }

  std::weak_ptr<MoqPeeringParticipant> owner_;
  moxygen::FullTrackName ftn_;
};

template <typename T> void MoqPeeringParticipant::spawn(folly::coro::Task<T> task) {
  if (!executor_) {
    return;
  }
  folly::coro::co_withExecutor(folly::getKeepAliveToken(executor_), std::move(task)).start();
}

MoqPeeringParticipant::MoqPeeringParticipant(
    std::shared_ptr<PeeringAnnouncer> announcer,
    std::shared_ptr<MoqxRelay> relay
)
    : announcer_(std::move(announcer)), relay_(std::move(relay)) {}

void MoqPeeringParticipant::prepare() {
  if (info_) {
    return;
  }
  auto info = std::make_shared<ParticipantInfo>();
  info->sessionId = announcer_->sessionId();
  info->version = moxygen::kVersionDraft18;
  info->executor = executor_;
  info->address = "moq-peering";
  info->routing = plainRouting({});
  info->routing.fromPeering = true;
  info_ = std::move(info);
}

Participant MoqPeeringParticipant::asParticipant() const {
  auto self = std::const_pointer_cast<MoqPeeringParticipant>(shared_from_this());
  return Participant(info_, self, self);
}

void MoqPeeringParticipant::attach() {
  if (attached_) {
    return;
  }
  attached_ = true;
  auto relay = relay_.lock();
  if (!relay) {
    return;
  }
  relay->deferOnRelay([self = shared_from_this()] {
    if (auto relay = self->relay_.lock(); relay && relay->getRelayExec()) {
      self->executor_ = relay->getRelayExec();
    } else {
      self->executor_ = &callingThreadExecutor();
    }
    self->prepare();
    self->spawn(self->attachOnRelay());
  });
}

void MoqPeeringParticipant::detach() {
  auto relay = relay_.lock();
  if (!relay) {
    return;
  }
  relay->deferOnRelay([self = shared_from_this()] {
    if (auto relay = self->relay_.lock()) {
      relay->clearPeeringFallback();
    }
    for (const auto& ftn : self->announcedTracks_) {
      self->announcer_->withdrawTrack(ftn);
    }
    self->announcedTracks_.clear();
    for (auto& [ns, state] : self->announcedNamespaces_) {
      if (state.announced) {
        self->announcer_->withdrawNamespace(ns);
      }
    }
    self->announcedNamespaces_.clear();
    self->namespaceSub_.reset();
    self->tracksSub_.reset();
    self->ingress_.clear();
    self->pullConsumers_.clear();
    self->ingressStreams_.clear();
    self->egress_.clear();
    self->egressForward_.clear();
    self->ready_.store(false, std::memory_order_release);
  });
}

folly::coro::Task<void> MoqPeeringParticipant::attachOnRelay() {
  auto relay = relay_.lock();
  if (!relay) {
    co_return;
  }
  ParticipantScope scope(asParticipant());
  moxygen::SubscribeNamespace subNs;
  subNs.requestID = moxygen::RequestID(nextRequest_++);
  subNs.forward = false;
  subNs.options = moxygen::SubscribeNamespaceOptions::NAMESPACE;
  auto ns = co_await relay->subscribeNamespace(std::move(subNs), shared_from_this());
  if (ns.hasError()) {
    XLOG(WARNING) << "moq-peering namespace subscribe failed: " << ns.error().reasonPhrase;
  } else {
    namespaceSub_ = std::move(ns.value());
  }

  moxygen::SubscribeTracks subTracks;
  subTracks.requestID = moxygen::RequestID(nextRequest_++);
  subTracks.forward = false;
  auto tracks = co_await relay->subscribeTracks(std::move(subTracks));
  if (tracks.hasError()) {
    XLOG(WARNING) << "moq-peering tracks subscribe failed: " << tracks.error().reasonPhrase;
  } else {
    tracksSub_ = std::move(tracks.value());
  }
  // A local subscribe can arrive before any peer has announced. Pull that track
  // now so the publisher's relay already has a subscriber when the publisher starts.
  relay->setPeeringFallback(asParticipant());
  ready_.store(true, std::memory_order_release);
}

void MoqPeeringParticipant::onNamespace(moxygen::TrackNamespace ns, bool withdrawn) {
  if (withdrawn) {
    withdrawPeerNamespace(ns);
    return;
  }
  spawn(publishPeerNamespace(std::move(ns)));
}

void MoqPeeringParticipant::onTrack(moxygen::FullTrackName ftn, bool withdrawn) {
  if (withdrawn) {
    endPeerTrack(ftn);
    return;
  }
  spawn(publishPeerTrack(std::move(ftn)));
}

folly::coro::Task<void> MoqPeeringParticipant::publishPeerNamespace(moxygen::TrackNamespace ns) {
  if (auto existing = peerNamespaces_.find(ns); existing != peerNamespaces_.end()) {
    while (existing->second.handle == nullptr && !existing->second.cancel) {
      co_await folly::coro::co_reschedule_on_current_executor;
      existing = peerNamespaces_.find(ns);
      if (existing == peerNamespaces_.end()) {
        co_return;
      }
    }
    co_return;
  }
  peerNamespaces_.emplace(ns, PeerNamespace{});
  auto relay = relay_.lock();
  if (!relay) {
    peerNamespaces_.erase(ns);
    co_return;
  }
  ParticipantScope scope(asParticipant());
  moxygen::PublishNamespace pub;
  pub.requestID = moxygen::RequestID(nextRequest_++);
  pub.trackNamespace = ns;
  auto result = co_await relay->publishNamespace(std::move(pub), nullptr);
  auto it = peerNamespaces_.find(ns);
  if (it == peerNamespaces_.end()) {
    co_return;
  }
  if (result.hasError()) {
    XLOG(WARNING) << "moq-peering namespace rejected ns=" << safeName(ns)
                  << " reason=" << result.error().reasonPhrase;
    peerNamespaces_.erase(it);
    co_return;
  }
  if (it->second.cancel) {
    result.value()->publishNamespaceDone();
    peerNamespaces_.erase(it);
    co_return;
  }
  it->second.handle = std::move(result.value());
}

folly::coro::Task<void> MoqPeeringParticipant::publishPeerTrack(moxygen::FullTrackName ftn) {
  if (ingress_.contains(ftn) || originated_.contains(ftn)) {
    co_return;
  }
  originated_.insert(ftn);
  co_await publishPeerNamespace(ftn.trackNamespace);
  auto relay = relay_.lock();
  if (!relay || !originated_.contains(ftn)) {
    co_return;
  }
  ParticipantScope scope(asParticipant());
  moxygen::PublishRequest pub;
  pub.requestID = moxygen::RequestID(nextRequest_++);
  pub.fullTrackName = ftn;
  pub.trackAlias = moxygen::TrackAlias(pub.requestID.value);
  pub.forward = true;
  pub.groupOrder = moxygen::GroupOrder::OldestFirst;
  auto handle = std::make_shared<PeeringIngressHandle>(weak_from_this(), ftn, pub.requestID);
  auto published =
      relay->createSubscriberFilter()->publish(std::move(pub), std::move(handle));
  if (published.hasError()) {
    if (published.error().reasonPhrase == kTrackAlreadyServed) {
      // A local subscribe already pulled this track. Keep that subscription.
      XLOG(INFO) << "moq-peering injected track ftn=" << safeName(ftn);
      co_return;
    }
    XLOG(WARNING) << "moq-peering track rejected ftn=" << safeName(ftn)
                  << " reason=" << published.error().reasonPhrase;
    originated_.erase(ftn);
    co_return;
  }
  if (!originated_.contains(ftn)) {
    co_return;
  }
  ingress_[ftn] = published->consumer;
  XLOG(INFO) << "moq-peering injected track ftn=" << safeName(ftn);
  auto reply = co_await std::move(published->reply);
  if (reply.hasError()) {
    if (reply.error().reasonPhrase == kTrackAlreadyServed) {
      ingress_.erase(ftn);
      co_return;
    }
    XLOG(WARNING) << "moq-peering track registration failed ftn=" << safeName(ftn)
                  << " reason=" << reply.error().reasonPhrase;
    co_return;
  }
  // Subscribers already attached are reported on PUBLISH_OK, not a later update.
  if (reply.value().forward) {
    pull(ftn);
  }
}

void MoqPeeringParticipant::withdrawPeerNamespace(const moxygen::TrackNamespace& ns) {
  auto it = peerNamespaces_.find(ns);
  if (it == peerNamespaces_.end()) {
    return;
  }
  if (!it->second.handle) {
    it->second.cancel = true;
    return;
  }
  it->second.handle->publishNamespaceDone();
  peerNamespaces_.erase(it);
}

void MoqPeeringParticipant::endPeerTrack(const moxygen::FullTrackName& ftn) {
  originated_.erase(ftn);
  unpull(ftn);
  auto streams = ingressStreams_.begin();
  while (streams != ingressStreams_.end()) {
    if (streams->first.first == ftn) {
      for (const auto& consumer : streams->second) {
        if (consumer) {
          consumer->reset(moxygen::ResetStreamErrorCode::CANCELLED);
        }
      }
      streams = ingressStreams_.erase(streams);
    } else {
      ++streams;
    }
  }
  auto it = ingress_.find(ftn);
  if (it == ingress_.end()) {
    return;
  }
  auto consumer = std::move(it->second);
  ingress_.erase(it);
  if (!consumer) {
    return;
  }
  auto done = consumer->publishDone(
      moxygen::PublishDone{
          .requestID = moxygen::RequestID(0),
          .statusCode = moxygen::PublishDoneStatusCode::TRACK_ENDED,
          .streamCount = 0,
          .reasonPhrase = "peering withdraw"
      }
  );
  if (done.hasError()) {
    XLOG(DBG1) << "moq-peering track withdraw failed ftn=" << safeName(ftn);
  }
}

void MoqPeeringParticipant::onSubscribe(const moxygen::FullTrackName& ftn) {
  XLOG(INFO) << "moq-peering subscribe ftn=" << safeName(ftn);
  setEgressForward(ftn, true);
}

void MoqPeeringParticipant::onUnsubscribe(const moxygen::FullTrackName& ftn) {
  XLOG(INFO) << "moq-peering unsubscribe ftn=" << safeName(ftn);
  setEgressForward(ftn, false);
}

void MoqPeeringParticipant::pull(const moxygen::FullTrackName& ftn) {
  if (!pulled_.insert(ftn).second) {
    return;
  }
  XLOG(INFO) << "moq-peering subscribe track ftn=" << safeName(ftn);
  announcer_->subscribeTrack(ftn);
}

void MoqPeeringParticipant::unpull(const moxygen::FullTrackName& ftn) {
  if (pulled_.erase(ftn) == 0) {
    return;
  }
  XLOG(INFO) << "moq-peering unsubscribe track ftn=" << safeName(ftn);
  announcer_->unsubscribeTrack(ftn);
}

void MoqPeeringParticipant::setEgressForward(const moxygen::FullTrackName& ftn, bool forward) {
  egressForward_[ftn] = forward;
  auto it = egress_.find(ftn);
  if (it == egress_.end() || !it->second) {
    return;
  }
  moxygen::RequestUpdate update;
  update.forward = forward;
  auto handle = it->second;
  // The task owns the handle. publishDone can drop egress_ before it runs.
  spawn(updateEgress(std::move(handle), std::move(update)));
}

void MoqPeeringParticipant::sendLocalObject(const moxygen::FullTrackName& ftn, PeeringObject object) {
  if (object.payload.size() > kMaxPeeringPayload) {
    XLOG(WARNING) << "moq-peering dropped an object larger than 1MiB";
    return;
  }
  announcer_->sendObject(ftn, std::move(object));
}

void MoqPeeringParticipant::endLocalSubgroup(
    const moxygen::FullTrackName& ftn,
    uint64_t group,
    uint64_t subgroup,
    bool reset
) {
  announcer_->endSubgroup(ftn, group, subgroup, reset);
}

void MoqPeeringParticipant::announceNamespaceOnce(
    const moxygen::TrackNamespace& ns,
    bool fromSubscription
) {
  auto& state = announcedNamespaces_[ns];
  if (fromSubscription) {
    state.subscription = true;
  }
  if (state.announced) {
    return;
  }
  state.announced = true;
  XLOG(INFO) << "moq-peering announce namespace ns=" << safeName(ns);
  announcer_->announceNamespace(ns);
}

void MoqPeeringParticipant::withdrawAnnouncedNamespace(const moxygen::TrackNamespace& ns) {
  auto it = announcedNamespaces_.find(ns);
  if (it == announcedNamespaces_.end() || !it->second.announced) {
    return;
  }
  it->second.announced = false;
  it->second.subscription = false;
  XLOG(INFO) << "moq-peering withdraw namespace ns=" << safeName(ns);
  announcer_->withdrawNamespace(ns);
}

void MoqPeeringParticipant::namespaceMsg(const moxygen::TrackNamespace& trackNamespaceSuffix) {
  announceNamespaceOnce(trackNamespaceSuffix, true);
}

void MoqPeeringParticipant::namespaceDoneMsg(const moxygen::TrackNamespace& trackNamespaceSuffix) {
  auto relay = relay_.lock();
  if (!relay) {
    return;
  }
  const auto session = info_ ? info_->sessionId : moxygen::SessionId{};
  for (const auto id : relay->findNamespacePublishers(trackNamespaceSuffix)) {
    if (id != session) {
      return;
    }
  }
  withdrawAnnouncedNamespace(trackNamespaceSuffix);
}

void MoqPeeringParticipant::onLocalTrackDone(const moxygen::FullTrackName& ftn) {
  egress_.erase(ftn);
  egressForward_.erase(ftn);
  if (!announcedTracks_.erase(ftn)) {
    return;
  }
  XLOG(INFO) << "moq-peering withdraw track ftn=" << safeName(ftn);
  announcer_->withdrawTrack(ftn);
  for (const auto& announced : announcedTracks_) {
    if (announced.trackNamespace == ftn.trackNamespace) {
      return;
    }
  }
  auto ns = announcedNamespaces_.find(ftn.trackNamespace);
  if (ns != announcedNamespaces_.end() && ns->second.subscription) {
    return;
  }
  withdrawAnnouncedNamespace(ftn.trackNamespace);
}

folly::coro::Task<moxygen::Publisher::SubscribeResult> MoqPeeringParticipant::subscribe(
    moxygen::SubscribeRequest sub,
    std::shared_ptr<moxygen::TrackConsumer> callback
) {
  // A peered namespace is published before its track. A local subscribe in that
  // window is asking us for the track, including one we are about to inject.
  auto handle = std::make_shared<PeeringPullHandle>(
      weak_from_this(),
      sub.fullTrackName,
      sub.requestID,
      sub.forward
  );
  if (callback) {
    handle->setConsumer(callback);
    pullConsumers_[sub.fullTrackName].push_back(std::move(callback));
  }
  co_return handle;
}

moxygen::Subscriber::PublishResult MoqPeeringParticipant::publish(
    moxygen::PublishRequest pub,
    std::shared_ptr<SubscriptionHandle> handle
) {
  if (originated_.contains(pub.fullTrackName)) {
    return folly::makeUnexpected(
        moxygen::PublishError{
            pub.requestID,
            moxygen::PublishErrorCode::INTERNAL_ERROR,
            "moq-peering does not subscribe to its own publish"
        }
    );
  }
  announceNamespaceOnce(pub.fullTrackName.trackNamespace, false);
  if (announcedTracks_.insert(pub.fullTrackName).second) {
    XLOG(INFO) << "moq-peering announce track ftn=" << safeName(pub.fullTrackName);
    announcer_->announceTrack(pub.fullTrackName);
  }
  if (!egressForward_.contains(pub.fullTrackName)) {
    egressForward_[pub.fullTrackName] = false;
  }
  egress_[pub.fullTrackName] = handle;
  auto self = weak_from_this();
  auto ftn = pub.fullTrackName;
  auto requestID = pub.requestID;
  auto consumer = std::make_shared<PeeringEgressTrack>(weak_from_this(), pub.fullTrackName);
  return moxygen::Subscriber::PublishConsumerAndReplyTask{
      std::move(consumer),
      folly::coro::co_invoke([self = std::move(self), ftn = std::move(ftn), requestID]() mutable
                             -> folly::coro::Task<folly::Expected<moxygen::PublishOk, moxygen::PublishError>> {
        moxygen::PublishOk ok;
        ok.requestID = requestID;
        ok.forward = false;
        if (auto owner = self.lock()) {
          if (auto it = owner->egressForward_.find(ftn); it != owner->egressForward_.end()) {
            ok.forward = it->second;
          }
        }
        co_return folly::Expected<moxygen::PublishOk, moxygen::PublishError>(std::move(ok));
      }),
      true
  };
}

std::vector<std::shared_ptr<moxygen::TrackConsumer>>
MoqPeeringParticipant::ingressTargets(const moxygen::FullTrackName& ftn) const {
  std::vector<std::shared_ptr<moxygen::TrackConsumer>> targets;
  if (auto it = ingress_.find(ftn); it != ingress_.end() && it->second) {
    targets.push_back(it->second);
  }
  if (auto it = pullConsumers_.find(ftn); it != pullConsumers_.end()) {
    for (const auto& consumer : it->second) {
      if (consumer) {
        targets.push_back(consumer);
      }
    }
  }
  return targets;
}

void MoqPeeringParticipant::writeObject(const moxygen::FullTrackName& ftn, const PeeringObject& object) {
  if (object.payload.size() > kMaxPeeringPayload) {
    XLOG(WARNING) << "moq-peering dropped an object larger than 1MiB";
    return;
  }
  auto header = headerFrom(object);
  auto payload = folly::IOBuf::copyBuffer(object.payload.data(), object.payload.size());
  for (const auto& consumer : ingressTargets(ftn)) {
    folly::Expected<folly::Unit, moxygen::MoQPublishError> result = folly::unit;
    if (object.datagram) {
      result = consumer->datagram(header, payload ? payload->clone() : nullptr, object.lastInGroup);
    } else {
      result =
          consumer->objectStream(header, payload ? payload->clone() : nullptr, object.lastInGroup);
    }
    if (result.hasError()) {
      XLOG(DBG1) << "moq-peering object not delivered ftn=" << safeName(ftn);
    }
  }
}

void MoqPeeringParticipant::deliverObject(
    const moxygen::FullTrackName& ftn,
    const PeeringObject& object
) {
  writeObject(ftn, object);
}

void MoqPeeringParticipant::acceptEvent(
    const moxygen::FullTrackName& ftn,
    uint64_t streamId,
    const PeeringStreamReader::Event& event
) {
  const auto key = std::make_pair(ftn, streamId);
  if (event.kind == PeeringStreamReader::Event::Kind::End) {
    finishIngress(ftn, streamId, false);
    return;
  }
  if (event.kind == PeeringStreamReader::Event::Kind::Subgroup) {
    moxygen::BeginSubgroupOptions options;
    options.includeExtensions = event.options.hasExtensions;
    options.subgroupIDFormat = event.options.subgroupIDFormat;
    options.containsLastInGroup = event.options.hasEndOfGroup;
    options.beginsWithFirstObject = event.options.beginsWithFirstObject;
    std::vector<std::shared_ptr<moxygen::SubgroupConsumer>> opened;
    for (const auto& track : ingressTargets(ftn)) {
      auto subgroup = track->beginSubgroup(
          event.object.group,
          event.object.subgroup,
          event.object.priority,
          options
      );
      if (!subgroup) {
        XLOG(DBG1) << "moq-peering subgroup not opened ftn=" << safeName(ftn);
        continue;
      }
      opened.push_back(std::move(subgroup.value()));
    }
    if (!opened.empty()) {
      ingressStreams_[key] = std::move(opened);
    }
    return;
  }

  auto streams = ingressStreams_.find(key);
  if (streams == ingressStreams_.end()) {
    return;
  }
  auto payload = folly::IOBuf::copyBuffer(event.object.payload.data(), event.object.payload.size());
  const auto status = static_cast<moxygen::ObjectStatus>(event.object.status);
  bool closed = false;
  for (const auto& consumer : streams->second) {
    if (!consumer) {
      continue;
    }
    folly::Expected<folly::Unit, moxygen::MoQPublishError> result = folly::unit;
    if (status == moxygen::ObjectStatus::END_OF_GROUP) {
      result = consumer->endOfGroup(event.object.objectId);
      closed = true;
    } else if (status == moxygen::ObjectStatus::END_OF_TRACK) {
      result = consumer->endOfTrackAndGroup(event.object.objectId);
      closed = true;
    } else {
      result = consumer->object(
          event.object.objectId,
          payload ? payload->clone() : nullptr,
          event.object.extensions,
          event.fin
      );
      if (event.fin) {
        auto ended = consumer->endOfSubgroup();
        if (ended.hasError() && result.hasValue()) {
          result = ended;
        }
        closed = true;
      }
    }
    if (result.hasError()) {
      XLOG(DBG1) << "moq-peering object not delivered ftn=" << safeName(ftn);
    }
  }
  if (closed) {
    ingressStreams_.erase(key);
  }
}

void MoqPeeringParticipant::finishIngress(
    const moxygen::FullTrackName& ftn,
    uint64_t streamId,
    bool reset
) {
  auto it = ingressStreams_.find(std::make_pair(ftn, streamId));
  if (it == ingressStreams_.end()) {
    return;
  }
  auto consumers = std::move(it->second);
  ingressStreams_.erase(it);
  for (const auto& consumer : consumers) {
    if (!consumer) {
      continue;
    }
    if (reset) {
      consumer->reset(moxygen::ResetStreamErrorCode::CANCELLED);
      continue;
    }
    auto result = consumer->endOfSubgroup();
    if (result.hasError()) {
      XLOG(DBG1) << "moq-peering subgroup end failed ftn=" << safeName(ftn);
    }
  }
}

} // namespace openmoq::moqx
