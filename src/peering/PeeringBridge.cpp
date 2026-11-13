/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "peering/PeeringBridge.h"

#include "MoqxRelay.h"

#include <folly/logging/xlog.h>

#include <utility>

namespace openmoq::moqx {

PeeringBridge::PeeringBridge(std::shared_ptr<peering::MoqPeeringRuntime> runtime)
    : runtime_(std::move(runtime)) {}

PeeringBridge::~PeeringBridge() {
  detach();
}

void PeeringBridge::attach(const std::vector<std::shared_ptr<MoqxRelay>>& relays) {
  auto self = shared_from_this();
  std::vector<std::weak_ptr<MoqxRelay>> stored;
  stored.reserve(relays.size());
  for (const auto& relay : relays) {
    if (!relay) {
      continue;
    }
    relay->setPeeringAnnouncer(self);
    stored.push_back(relay);
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    relays_ = std::move(stored);
    detached_ = false;
  }
  if (auto runtime = runtime_.lock()) {
    runtime->setEvents(this);
  }
}

void PeeringBridge::detach() {
  std::vector<std::shared_ptr<MoqxRelay>> relays;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (detached_) {
      return;
    }
    detached_ = true;
    relays.reserve(relays_.size());
    for (const auto& relay : relays_) {
      if (auto locked = relay.lock()) {
        relays.push_back(std::move(locked));
      }
    }
    relays_.clear();
  }
  if (auto runtime = runtime_.lock()) {
    runtime->setEvents(nullptr);
  }
  for (const auto& relay : relays) {
    relay->setPeeringAnnouncer(nullptr);
  }
}

void PeeringBridge::announceNamespace(const moxygen::TrackNamespace& ns) {
  if (auto runtime = runtime_.lock()) {
    runtime->announceNamespace(ns.trackNamespace);
  }
}

void PeeringBridge::withdrawNamespace(const moxygen::TrackNamespace& ns) {
  if (auto runtime = runtime_.lock()) {
    runtime->withdrawNamespace(ns.trackNamespace);
  }
}

void PeeringBridge::announceTrack(const moxygen::FullTrackName& ftn) {
  if (auto runtime = runtime_.lock()) {
    runtime->announceTrack(ftn.trackNamespace.trackNamespace, ftn.trackName);
  }
}

void PeeringBridge::withdrawTrack(const moxygen::FullTrackName& ftn) {
  if (auto runtime = runtime_.lock()) {
    runtime->withdrawTrack(ftn.trackNamespace.trackNamespace, ftn.trackName);
  }
}

void PeeringBridge::sendObject(const moxygen::FullTrackName& ftn, PeeringObject object) {
  auto runtime = runtime_.lock();
  if (!runtime) {
    return;
  }
  auto hash = runtime->hashTrack(ftn.trackNamespace.trackNamespace, ftn.trackName);
  if (!hash) {
    return;
  }
  std::optional<std::vector<uint8_t>> encoded;
  peering::MoqPeeringRuntime::WireKind kind = peering::MoqPeeringRuntime::WireKind::Datagram;
  bool finish = false;
  bool resetStream = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (detached_) {
      return;
    }
    if (object.datagram) {
      encoded = encodePeeringDatagram(*hash, object);
    } else {
      StreamKey key{*hash, object.group, object.subgroup};
      auto it = writers_.find(key);
      const bool start = it == writers_.end();
      if (start) {
        it = writers_.emplace(key, std::make_unique<PeeringSubgroupWriter>(*hash)).first;
      }
      encoded = it->second->write(object, start);
      if (!encoded) {
        writers_.erase(it);
        resetStream = !start;
        if (!resetStream) {
          XLOG(WARNING) << "moq-peering dropped an object that could not be encoded";
          return;
        }
      } else {
        kind = start ? peering::MoqPeeringRuntime::WireKind::NewStream
                     : peering::MoqPeeringRuntime::WireKind::ExistingStream;
        if (object.lastInGroup) {
          writers_.erase(key);
          finish = true;
        }
      }
    }
  }
  if (resetStream) {
    XLOG(WARNING) << "moq-peering reset a subgroup after an object failed to encode";
    runtime->endSubgroup(
        ftn.trackNamespace.trackNamespace,
        ftn.trackName,
        object.group,
        object.subgroup,
        true
    );
    return;
  }
  if (!encoded || encoded->empty()) {
    XLOG(WARNING) << "moq-peering dropped an object that could not be encoded";
    return;
  }
  auto bytes = std::make_shared<const std::vector<uint8_t>>(std::move(*encoded));
  runtime->sendData(
      ftn.trackNamespace.trackNamespace,
      ftn.trackName,
      object.priority,
      kind,
      object.group,
      object.subgroup,
      std::move(bytes)
  );
  if (finish) {
    runtime->endSubgroup(
        ftn.trackNamespace.trackNamespace,
        ftn.trackName,
        object.group,
        object.subgroup,
        false
    );
  }
}

void PeeringBridge::endSubgroup(
    const moxygen::FullTrackName& ftn,
    uint64_t group,
    uint64_t subgroup,
    bool reset
) {
  auto runtime = runtime_.lock();
  if (!runtime) {
    return;
  }
  auto hash = runtime->hashTrack(ftn.trackNamespace.trackNamespace, ftn.trackName);
  if (!hash) {
    return;
  }
  bool open = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    open = writers_.erase(StreamKey{*hash, group, subgroup}) > 0;
  }
  if (!open) {
    return;
  }
  runtime->endSubgroup(ftn.trackNamespace.trackNamespace, ftn.trackName, group, subgroup, reset);
}

void PeeringBridge::subscribeTrack(const moxygen::FullTrackName& ftn) {
  auto runtime = runtime_.lock();
  if (!runtime) {
    return;
  }
  auto hash = runtime->hashTrack(ftn.trackNamespace.trackNamespace, ftn.trackName);
  if (!hash) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (detached_) {
      return;
    }
    pulls_[*hash] = ftn;
  }
  runtime->subscribe(
      ftn.trackNamespace.trackNamespace,
      ftn.trackName,
      moxygen::kDefaultPriority,
      true
  );
}

void PeeringBridge::unsubscribeTrack(const moxygen::FullTrackName& ftn) {
  auto runtime = runtime_.lock();
  if (!runtime) {
    return;
  }
  if (auto hash = runtime->hashTrack(ftn.trackNamespace.trackNamespace, ftn.trackName)) {
    std::lock_guard<std::mutex> lock(mu_);
    pulls_.erase(*hash);
  }
  runtime->unsubscribe(ftn.trackNamespace.trackNamespace, ftn.trackName);
}

void PeeringBridge::onNamespace(std::vector<std::string> nameSpace, bool withdrawn) {
  moxygen::TrackNamespace ns{std::move(nameSpace)};
  dispatch([ns = std::move(ns), withdrawn](MoqxRelay& relay) {
    relay.injectPeeringNamespace(ns, withdrawn);
  });
}

void PeeringBridge::onTrack(std::vector<std::string> nameSpace, std::string track, bool withdrawn) {
  moxygen::FullTrackName ftn{moxygen::TrackNamespace{std::move(nameSpace)}, std::move(track)};
  dispatch([ftn = std::move(ftn), withdrawn](MoqxRelay& relay) {
    relay.injectPeeringTrack(ftn, withdrawn);
  });
}

void PeeringBridge::onSubscribe(
    std::vector<std::string> nameSpace,
    std::string track,
    uint64_t trackHash,
    uint8_t /*priority*/,
    uint8_t /*forward*/
) {
  // forward=0 is still a subscriber. LAPS resumes a paused publisher for every
  // peer subscribe. Dropping forward=0 left PUBLISH_OK forward=false in place,
  // so the publisher never wrote the objects the peering session would send.
  moxygen::FullTrackName ftn{moxygen::TrackNamespace{std::move(nameSpace)}, std::move(track)};
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (detached_) {
      return;
    }
    peerSubs_[trackHash] = ftn;
  }
  dispatch([ftn](MoqxRelay& relay) { relay.notePeerSubscribe(ftn); });
}

void PeeringBridge::onUnsubscribe(uint64_t trackHash, bool subscribersRemain) {
  if (subscribersRemain) {
    return;
  }
  moxygen::FullTrackName ftn;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = peerSubs_.find(trackHash);
    if (it == peerSubs_.end()) {
      return;
    }
    ftn = it->second;
    peerSubs_.erase(it);
  }
  dispatch([ftn](MoqxRelay& relay) { relay.notePeerUnsubscribe(ftn); });
}

void PeeringBridge::onData(
    uint64_t trackHash,
    bool isNewStream,
    std::optional<uint64_t> streamId,
    std::vector<uint8_t> bytes
) {
  moxygen::FullTrackName ftn;
  std::optional<PeeringObject> datagram;
  std::vector<PeeringStreamReader::Event> events;
  bool replaced = false;
  bool broken = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (detached_) {
      return;
    }
    auto it = pulls_.find(trackHash);
    if (it == pulls_.end()) {
      return;
    }
    ftn = it->second;
    if (!streamId) {
      datagram = decodePeeringDatagram(bytes.data(), bytes.size());
      if (!datagram) {
        XLOG(WARNING) << "moq-peering dropped a datagram that could not be decoded";
        return;
      }
    } else {
      const auto key = std::make_pair(trackHash, *streamId);
      if (isNewStream) {
        replaced = readers_.contains(key);
        readers_[key] = std::make_unique<PeeringStreamReader>();
      }
      auto reader = readers_.find(key);
      if (reader == readers_.end()) {
        XLOG(WARNING) << "moq-peering dropped stream bytes with no open stream";
        return;
      }
      if (!reader->second->ingest(bytes.data(), bytes.size(), false)) {
        readers_.erase(reader);
        broken = true;
        XLOG(WARNING) << "moq-peering dropped a stream that could not be decoded";
      } else {
        events = reader->second->take();
      }
    }
  }
  if (streamId && (replaced || broken)) {
    const uint64_t id = *streamId;
    dispatch([ftn, id](MoqxRelay& relay) { relay.finishPeeringIngress(ftn, id, true); });
    if (broken) {
      return;
    }
  }
  if (datagram) {
    dispatch([ftn, object = std::move(*datagram)](MoqxRelay& relay) {
      relay.deliverPeeringObject(ftn, object);
    });
  }
  if (!streamId) {
    return;
  }
  const uint64_t id = *streamId;
  for (auto& event : events) {
    dispatch([ftn, id, event = std::move(event)](MoqxRelay& relay) {
      relay.acceptPeeringEvent(ftn, id, event);
    });
  }
}

void PeeringBridge::onStreamClosed(uint64_t trackHash, uint64_t streamId, bool reset) {
  moxygen::FullTrackName ftn;
  std::vector<PeeringStreamReader::Event> events;
  bool known = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (detached_) {
      return;
    }
    auto it = pulls_.find(trackHash);
    if (it == pulls_.end()) {
      return;
    }
    ftn = it->second;
    auto reader = readers_.find(std::make_pair(trackHash, streamId));
    if (reader == readers_.end()) {
      return;
    }
    known = true;
    const bool parsed = !reset && reader->second->ingest(nullptr, 0, true);
    if (parsed) {
      events = reader->second->take();
    } else if (!reset) {
      reset = true;
    }
    readers_.erase(reader);
  }
  if (!known) {
    return;
  }
  if (reset) {
    dispatch([ftn, streamId](MoqxRelay& relay) {
      relay.finishPeeringIngress(ftn, streamId, true);
    });
    return;
  }
  for (auto& event : events) {
    dispatch([ftn, streamId, event = std::move(event)](MoqxRelay& relay) {
      relay.acceptPeeringEvent(ftn, streamId, event);
    });
  }
  dispatch([ftn, streamId](MoqxRelay& relay) { relay.finishPeeringIngress(ftn, streamId, false); });
}

void PeeringBridge::dispatch(std::function<void(MoqxRelay&)> fn) {
  std::vector<std::shared_ptr<MoqxRelay>> relays;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (detached_) {
      return;
    }
    relays.reserve(relays_.size());
    for (const auto& relay : relays_) {
      if (auto locked = relay.lock()) {
        relays.push_back(std::move(locked));
      }
    }
  }
  for (const auto& relay : relays) {
    relay->deferOnRelay([relay, fn]() { fn(*relay); });
  }
}

} // namespace openmoq::moqx
