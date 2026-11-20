/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "peering/MoqPeeringBackend.h"

#include "MoqxRelay.h"
#include "config/PeerEndpoint.h"
#include "relay/PlainRouteCodec.h"

#include <folly/logging/xlog.h>

#include <charconv>
#include <utility>

namespace openmoq::moqx {
namespace {

constexpr size_t kMaxPeerTokenBytes = 300;

bool knownEncoding(uint8_t encoding) {
  return encoding == 16 || encoding == 18 || encoding == 22;
}

std::optional<uint64_t> moqtVersion(uint8_t encoding) {
  switch (encoding) {
  case 16:
    return moxygen::kVersionDraft16;
  case 18:
    return moxygen::kVersionDraft18;
  case 22:
    return moxygen::kVersionDraft22;
  default:
    return std::nullopt;
  }
}

std::optional<std::pair<std::string, uint16_t>> parsePeerToken(std::string_view peer) {
  if (peer.empty() || peer.size() > kMaxPeerTokenBytes) {
    return std::nullopt;
  }
  std::string_view host;
  std::string_view portText;
  if (peer.front() == '[') {
    const auto end = peer.find(']');
    if (end == std::string_view::npos || end + 2 > peer.size() || peer[end + 1] != ':') {
      return std::nullopt;
    }
    host = peer.substr(1, end - 1);
    portText = peer.substr(end + 2);
  } else {
    const auto colon = peer.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 == peer.size()) {
      return std::nullopt;
    }
    host = peer.substr(0, colon);
    portText = peer.substr(colon + 1);
  }
  uint32_t port = 0;
  const auto* first = portText.data();
  const auto parsed = std::from_chars(first, first + portText.size(), port);
  if (parsed.ec != std::errc{} || parsed.ptr != first + portText.size()) {
    return std::nullopt;
  }
  if (config::validatePeerPort(port)) {
    return std::nullopt;
  }
  std::string normalized;
  if (config::normalizePeerHost(host, normalized)) {
    return std::nullopt;
  }
  return std::make_pair(std::move(normalized), static_cast<uint16_t>(port));
}

PeerAdminResult::Error errorFor(const peering::PeerCommandResult& result) {
  if (result.ok) {
    return PeerAdminResult::Error::None;
  }
  if (result.message.find("does not exist") != std::string::npos) {
    return PeerAdminResult::Error::NotFound;
  }
  if (result.message.find("duplicate") != std::string::npos) {
    return PeerAdminResult::Error::Duplicate;
  }
  return PeerAdminResult::Error::Invalid;
}

PeerLink::State linkState(std::string_view status) {
  if (status == "connected") {
    return PeerLink::State::Connected;
  }
  if (status == "disconnected") {
    return PeerLink::State::Disconnected;
  }
  return PeerLink::State::Connecting;
}

} // namespace

std::string moqPeerToken(std::string_view host, uint16_t port) {
  std::string out;
  if (host.find(':') != std::string_view::npos) {
    out.reserve(host.size() + 8);
    out.push_back('[');
    out.append(host);
    out.push_back(']');
  } else {
    out.assign(host);
  }
  out.push_back(':');
  out.append(std::to_string(port));
  return out;
}

MoqPeeringCore::MoqPeeringCore(std::shared_ptr<peering::MoqPeeringRuntime> runtime)
    : runtime_(std::move(runtime)) {}

MoqPeeringCore::~MoqPeeringCore() {
  if (auto runtime = runtime_.lock()) {
    runtime->setEvents(nullptr);
  }
}

void MoqPeeringCore::addRelay(const std::shared_ptr<MoqxRelay>& relay) {
  if (!relay) {
    return;
  }
  auto participant = std::make_shared<MoqPeeringParticipant>(shared_from_this(), relay);
  std::lock_guard<std::mutex> lock(mu_);
  participants_.push_back(std::move(participant));
}

void MoqPeeringCore::arm() {
  bool first = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!armed_) {
      armed_ = true;
      first = true;
    }
  }
  if (!first) {
    return;
  }
  if (auto runtime = runtime_.lock()) {
    runtime->setEvents(this);
  }
  std::vector<std::shared_ptr<MoqPeeringParticipant>> participants;
  {
    std::lock_guard<std::mutex> lock(mu_);
    participants = participants_;
  }
  for (const auto& participant : participants) {
    participant->attach();
  }
}

void MoqPeeringCore::detach(MoqxRelay& relay) {
  std::shared_ptr<MoqPeeringParticipant> gone;
  bool last = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    std::erase_if(participants_, [&](const std::shared_ptr<MoqPeeringParticipant>& participant) {
      auto held = participant->relay();
      if (held && held.get() == &relay) {
        gone = participant;
        return true;
      }
      return !held;
    });
    last = participants_.empty();
    if (last) {
      armed_ = false;
      writers_.clear();
      readers_.clear();
      peerSubs_.clear();
      pulls_.clear();
      peerSessions_.clear();
      restartStreams_ = false;
    }
  }
  if (gone) {
    gone->detach();
  }
  if (last) {
    if (auto runtime = runtime_.lock()) {
      runtime->setEvents(nullptr);
    }
  }
}

void MoqPeeringCore::disarm() {
  std::vector<std::shared_ptr<MoqPeeringParticipant>> participants;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!armed_ && participants_.empty()) {
      return;
    }
    armed_ = false;
    participants = std::move(participants_);
    writers_.clear();
    readers_.clear();
    peerSubs_.clear();
    pulls_.clear();
    peerSessions_.clear();
    restartStreams_ = false;
  }
  if (auto runtime = runtime_.lock()) {
    runtime->setEvents(nullptr);
  }
  for (const auto& participant : participants) {
    participant->detach();
  }
}

std::optional<peering::PeeringStatus> MoqPeeringCore::status() const {
  auto runtime = runtime_.lock();
  if (!runtime) {
    return std::nullopt;
  }
  return runtime->status();
}

PeerAdminResult MoqPeeringCore::execute(PeerAdminOp op) {
  auto runtime = runtime_.lock();
  if (!runtime) {
    return PeerAdminResult{
        .error = PeerAdminResult::Error::Invalid,
        .peer = std::move(op.peer),
        .metric = op.metric,
        .message = "moq-fabric is not running"
    };
  }
  auto parsed = parsePeerToken(op.peer);
  if (!parsed) {
    return PeerAdminResult{
        .error = PeerAdminResult::Error::Invalid,
        .peer = std::move(op.peer),
        .metric = op.metric,
        .message = "peer must be host:port"
    };
  }
  if (op.kind == PeerAdminOp::Kind::SetMetric && !op.metric) {
    return PeerAdminResult{
        .error = PeerAdminResult::Error::Invalid,
        .peer = std::move(op.peer),
        .message = "metric is required"
    };
  }
  peering::PeerCommandResult result;
  switch (op.kind) {
  case PeerAdminOp::Kind::Add:
    result = runtime->addPeer(parsed->first, parsed->second, op.metric);
    break;
  case PeerAdminOp::Kind::Remove:
    result = runtime->removePeer(parsed->first, parsed->second);
    break;
  case PeerAdminOp::Kind::Restart:
    result = runtime->restartPeer(parsed->first, parsed->second);
    break;
  case PeerAdminOp::Kind::SetMetric:
    result = runtime->setPeerMetric(parsed->first, parsed->second, *op.metric);
    break;
  }
  const auto& host = result.host.empty() ? parsed->first : result.host;
  const auto port = result.port == 0 ? parsed->second : result.port;
  return PeerAdminResult{
      .error = errorFor(result),
      .peer = moqPeerToken(host, port),
      .metric = result.metric ? result.metric : op.metric,
      .message = std::move(result.message),
      .action = std::move(result.action)
  };
}

std::vector<PeerLink> MoqPeeringCore::links() const {
  auto current = status();
  if (!current) {
    return {};
  }
  std::vector<PeerLink> out;
  out.reserve(current->peers.size());
  for (const auto& peer : current->peers) {
    PeerLink link;
    link.direction =
        peer.origin == "dialed" ? PeerLink::Direction::Dialed : PeerLink::Direction::Accepted;
    link.state = linkState(peer.status);
    link.address = moqPeerToken(peer.host, peer.port);
    link.authority = peer.contact;
    link.relayID = peer.nodeId;
    link.metrics = PeerMetrics{
        .srttUs = peer.srttUs,
        .metric = peer.metric,
        .nodeMetric = peer.nodeMetric,
        .connectTimeUs = peer.connectTimeUs,
        .subscribeTracks = peer.subscribeTracks,
        .publishes = peer.publishes,
        .activeStreams = peer.activeStreams
    };
    out.push_back(std::move(link));
  }
  return out;
}

std::optional<uint8_t> MoqPeeringCore::encodingFor(
    const moxygen::FullTrackName& ftn,
    const std::map<moxygen::FullTrackName, uint8_t>& tracks,
    const std::map<moxygen::TrackNamespace, uint8_t>& namespaces
) const {
  if (auto it = tracks.find(ftn); it != tracks.end()) {
    return it->second;
  }
  std::optional<uint8_t> found;
  size_t best = 0;
  for (const auto& [ns, encoding] : namespaces) {
    if (ftn.trackNamespace.startsWith(ns) && ns.trackNamespace.size() >= best) {
      best = ns.trackNamespace.size();
      found = encoding;
    }
  }
  return found;
}

void MoqPeeringCore::announceNamespace(const moxygen::TrackNamespace& ns, uint8_t encoding) {
  if (!knownEncoding(encoding)) {
    XLOG(WARNING) << "moq-fabric dropped a namespace announce with an unknown encoding";
    return;
  }
  auto runtime = runtime_.lock();
  if (!runtime) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    localNamespaces_.emplace(ns, encoding);
  }
  runtime->announceNamespace(ns.trackNamespace, encoding);
}

void MoqPeeringCore::withdrawNamespace(const moxygen::TrackNamespace& ns) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    localNamespaces_.erase(ns);
  }
  if (auto runtime = runtime_.lock()) {
    runtime->withdrawNamespace(ns.trackNamespace);
  }
}

void MoqPeeringCore::announceTrack(const moxygen::FullTrackName& ftn, uint8_t encoding) {
  if (!knownEncoding(encoding)) {
    XLOG(WARNING) << "moq-fabric dropped a track announce with an unknown encoding";
    return;
  }
  auto runtime = runtime_.lock();
  if (!runtime) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    localTracks_.emplace(ftn, encoding);
  }
  runtime->announceTrack(ftn.trackNamespace.trackNamespace, ftn.trackName, encoding);
}

void MoqPeeringCore::withdrawTrack(const moxygen::FullTrackName& ftn) {
  auto runtime = runtime_.lock();
  if (!runtime) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    localTracks_.erase(ftn);
  }
  // The replacement publish can reuse this group before the old subgroup
  // closes. Drop the writer so the next object is a new stream, then withdraw.
  std::vector<std::pair<uint64_t, uint64_t>> open;
  if (auto hash = runtime->hashTrack(ftn.trackNamespace.trackNamespace, ftn.trackName)) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = writers_.begin(); it != writers_.end();) {
      if (it->first.hash != *hash) {
        ++it;
        continue;
      }
      open.emplace_back(it->first.group, it->first.subgroup);
      it = writers_.erase(it);
    }
  }
  for (const auto& subgroup : open) {
    runtime->endSubgroup(
        ftn.trackNamespace.trackNamespace,
        ftn.trackName,
        subgroup.first,
        subgroup.second,
        true
    );
  }
  runtime->withdrawTrack(ftn.trackNamespace.trackNamespace, ftn.trackName);
}

void MoqPeeringCore::sendObject(const moxygen::FullTrackName& ftn, PeeringObject object) {
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
    if (!armed_) {
      return;
    }
    const auto encoding = encodingFor(ftn, localTracks_, localNamespaces_);
    const auto version = encoding ? moqtVersion(*encoding) : std::nullopt;
    if (!version) {
      XLOG(WARNING) << "moq-fabric dropped an object with no publish encoding";
      return;
    }
    if (object.datagram) {
      encoded = encodePeeringDatagram(*hash, object, *version);
    } else {
      StreamKey key{*hash, object.group, object.subgroup};
      // No live session owns the previous stream. Keep encoding this subgroup
      // as a new stream until one is actually open again.
      if (restartStreams_ || peerSessions_.empty()) {
        writers_.erase(key);
      }
      auto it = writers_.find(key);
      const bool start = it == writers_.end();
      if (start) {
        it = writers_.emplace(key, std::make_unique<PeeringSubgroupWriter>(*hash, *version)).first;
      }
      encoded = it->second->write(object, start);
      if (!encoded) {
        writers_.erase(it);
        resetStream = !start;
        if (!resetStream) {
          XLOG(WARNING) << "moq-fabric dropped an object that could not be encoded";
          return;
        }
      } else {
        kind = start ? peering::MoqPeeringRuntime::WireKind::NewStream
                     : peering::MoqPeeringRuntime::WireKind::ExistingStream;
        if (start && peerSessions_.empty()) {
          writers_.erase(key);
        } else if (start) {
          restartStreams_ = false;
        }
        if (object.lastInGroup) {
          writers_.erase(key);
          finish = true;
        }
      }
    }
  }
  if (resetStream) {
    XLOG(WARNING) << "moq-fabric reset a subgroup after an object failed to encode";
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
    XLOG(WARNING) << "moq-fabric dropped an object that could not be encoded";
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

void MoqPeeringCore::endSubgroup(
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

void MoqPeeringCore::subscribeTrack(const moxygen::FullTrackName& ftn) {
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
    if (!armed_) {
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

void MoqPeeringCore::unsubscribeTrack(const moxygen::FullTrackName& ftn) {
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

void MoqPeeringCore::onPeerUp(uint64_t sessionId) {
  if (sessionId == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(mu_);
  peerSessions_.insert(sessionId);
}

void MoqPeeringCore::onPeerDown(uint64_t sessionId) {
  std::vector<std::pair<moxygen::FullTrackName, uint64_t>> open;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (sessionId == 0) {
      // The dialer drops the connection before the id can be read. With one
      // session, that was the only peer. With several, the other sessions are
      // still named and keep their streams.
      if (peerSessions_.size() <= 1) {
        peerSessions_.clear();
      }
    } else {
      peerSessions_.erase(sessionId);
    }
    if (!peerSessions_.empty()) {
      return;
    }
    // Every open subgroup belonged to a session that is gone. The next object
    // on each one has to carry the subgroup header on a new stream.
    restartStreams_ = true;
    writers_.clear();
    for (const auto& [key, reader] : readers_) {
      (void)reader;
      auto pull = pulls_.find(key.first);
      if (pull != pulls_.end()) {
        open.emplace_back(pull->second, key.second);
      }
    }
    readers_.clear();
  }
  if (!open.empty()) {
    XLOG(INFO) << "moq-fabric closed " << open.size()
               << " ingress streams after the last peer session";
  }
  for (const auto& [ftn, streamId] : open) {
    dispatch([ftn, streamId](MoqPeeringParticipant& participant) {
      participant.finishIngress(ftn, streamId, true);
    });
  }
}

void MoqPeeringCore::onNamespace(
    std::vector<std::string> nameSpace,
    uint8_t encoding,
    bool withdrawn
) {
  moxygen::TrackNamespace ns{std::move(nameSpace)};
  if (!withdrawn && knownEncoding(encoding)) {
    std::lock_guard<std::mutex> lock(mu_);
    peerNamespaceEncodings_.emplace(ns, encoding);
  }
  dispatch([ns = std::move(ns), withdrawn](MoqPeeringParticipant& participant) {
    participant.onNamespace(ns, withdrawn);
  });
}

void MoqPeeringCore::onTrack(
    std::vector<std::string> nameSpace,
    std::string track,
    uint8_t encoding,
    bool withdrawn
) {
  moxygen::FullTrackName ftn{moxygen::TrackNamespace{std::move(nameSpace)}, std::move(track)};
  if (!withdrawn && knownEncoding(encoding)) {
    std::lock_guard<std::mutex> lock(mu_);
    peerTrackEncodings_.emplace(ftn, encoding);
  }
  dispatch([ftn = std::move(ftn), withdrawn](MoqPeeringParticipant& participant) {
    participant.onTrack(ftn, withdrawn);
  });
}

void MoqPeeringCore::onSubscribe(
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
    if (!armed_) {
      return;
    }
    peerSubs_[trackHash] = ftn;
  }
  dispatch([ftn](MoqPeeringParticipant& participant) { participant.onSubscribe(ftn); });
}

void MoqPeeringCore::onUnsubscribe(uint64_t trackHash, bool subscribersRemain) {
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
  dispatch([ftn](MoqPeeringParticipant& participant) { participant.onUnsubscribe(ftn); });
}

void MoqPeeringCore::onData(
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
    if (!armed_) {
      return;
    }
    auto it = pulls_.find(trackHash);
    if (it == pulls_.end()) {
      return;
    }
    ftn = it->second;
    const auto encoding = encodingFor(ftn, peerTrackEncodings_, peerNamespaceEncodings_);
    const auto version = encoding ? moqtVersion(*encoding) : std::nullopt;
    if (!version) {
      XLOG(WARNING) << "moq-fabric dropped peer data with no announce encoding";
      return;
    }
    if (!streamId) {
      datagram = decodePeeringDatagram(bytes.data(), bytes.size(), *version);
      if (!datagram) {
        XLOG(WARNING) << "moq-fabric dropped a datagram that could not be decoded";
        return;
      }
    } else {
      const auto key = std::make_pair(trackHash, *streamId);
      if (isNewStream) {
        replaced = readers_.contains(key);
        readers_[key] = std::make_unique<PeeringStreamReader>(*version);
      }
      auto reader = readers_.find(key);
      if (reader == readers_.end()) {
        XLOG(WARNING) << "moq-fabric dropped stream bytes with no open stream";
        return;
      }
      if (!reader->second->ingest(bytes.data(), bytes.size(), false)) {
        readers_.erase(reader);
        broken = true;
        XLOG(WARNING) << "moq-fabric dropped a stream that could not be decoded";
      } else {
        events = reader->second->take();
      }
    }
  }
  if (streamId && (replaced || broken)) {
    const uint64_t id = *streamId;
    dispatch([ftn, id](MoqPeeringParticipant& participant) {
      participant.finishIngress(ftn, id, true);
    });
    if (broken) {
      return;
    }
  }
  if (datagram) {
    dispatch([ftn, object = std::move(*datagram)](MoqPeeringParticipant& participant) {
      participant.deliverObject(ftn, object);
    });
  }
  if (!streamId) {
    return;
  }
  const uint64_t id = *streamId;
  for (auto& event : events) {
    dispatch([ftn, id, event = std::move(event)](MoqPeeringParticipant& participant) {
      participant.acceptEvent(ftn, id, event);
    });
  }
}

void MoqPeeringCore::onStreamClosed(uint64_t trackHash, uint64_t streamId, bool reset) {
  moxygen::FullTrackName ftn;
  std::vector<PeeringStreamReader::Event> events;
  bool known = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!armed_) {
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
    dispatch([ftn, streamId](MoqPeeringParticipant& participant) {
      participant.finishIngress(ftn, streamId, true);
    });
    return;
  }
  for (auto& event : events) {
    dispatch([ftn, streamId, event = std::move(event)](MoqPeeringParticipant& participant) {
      participant.acceptEvent(ftn, streamId, event);
    });
  }
  dispatch([ftn, streamId](MoqPeeringParticipant& participant) {
    participant.finishIngress(ftn, streamId, false);
  });
}

void MoqPeeringCore::dispatch(std::function<void(MoqPeeringParticipant&)> fn) {
  std::vector<std::shared_ptr<MoqPeeringParticipant>> participants;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!armed_) {
      return;
    }
    participants = participants_;
  }
  for (const auto& participant : participants) {
    auto relay = participant->relay();
    if (!relay) {
      continue;
    }
    relay->deferOnRelay([participant, fn]() { fn(*participant); });
  }
}

MoqPeeringBackend::MoqPeeringBackend(
    std::shared_ptr<MoqxRelay> relay,
    std::shared_ptr<MoqPeeringCore> core
)
    : relay_(std::move(relay)), core_(std::move(core)) {}

void MoqPeeringBackend::start(folly::EventBase* /*evb*/) {
  core_->arm();
}

void MoqPeeringBackend::stop() {
  if (auto relay = relay_.lock()) {
    core_->detach(*relay);
  }
}

folly::coro::Task<void> MoqPeeringBackend::waitForPeers(folly::Function<bool()> /*routed*/) {
  co_return;
}

ParticipantRouting MoqPeeringBackend::routingFor(const ParticipantInfo& info) const {
  return plainRouting(info.routing);
}

folly::coro::Task<void> MoqPeeringBackend::visitPeers(PeerVisitor& visitor) const {
  for (const auto& link : core_->links()) {
    visitor.onPeer(link);
  }
  co_return;
}

folly::coro::Task<PeerAdminResult> MoqPeeringBackend::executePeerAdminOp(PeerAdminOp op) {
  co_return core_->execute(std::move(op));
}

} // namespace openmoq::moqx
