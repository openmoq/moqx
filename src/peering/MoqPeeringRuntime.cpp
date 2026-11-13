/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "peering/MoqPeeringRuntime.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <utility>

#include <moq/peering/peer_manager.h>

namespace openmoq::moqx::peering {
namespace {

std::atomic<PeeringLogFn> gLogger{nullptr};

void emitLog(PeeringLogLevel level, std::string message) {
  if (message.size() > 512) {
    message.resize(512);
  }
  for (char& c : message) {
    if (c == '\n' || c == '\r') {
      c = ' ';
    }
  }
  if (PeeringLogFn fn = gLogger.load(std::memory_order_acquire)) {
    fn(level, message.c_str());
  }
}

// Matches the default written into resolved config when node_metric is omitted.
static_assert(
    moq::peering::kDefaultNodeMetric == 100,
    "MoqPeeringConfig::nodeMetric default must match moq-peering"
);

moq::peering::NodeType toLibrary(config::MoqPeeringNodeType type) {
  switch (type) {
  case config::MoqPeeringNodeType::Via:
    return moq::peering::NodeType::kVia;
  case config::MoqPeeringNodeType::Edge:
    return moq::peering::NodeType::kEdge;
  case config::MoqPeeringNodeType::Stub:
    return moq::peering::NodeType::kStub;
  }
  return moq::peering::NodeType::kEdge;
}

const char* nodeTypeName(moq::peering::NodeType type) {
  switch (type) {
  case moq::peering::NodeType::kVia:
    return "via";
  case moq::peering::NodeType::kEdge:
    return "edge";
  case moq::peering::NodeType::kStub:
    return "stub";
  }
  return "edge";
}

const char* nodeTypeName(config::MoqPeeringNodeType type) {
  return nodeTypeName(toLibrary(type));
}

const char* originName(moq::peering::PeerOrigin origin) {
  switch (origin) {
  case moq::peering::PeerOrigin::kAccepted:
    return "accepted";
  case moq::peering::PeerOrigin::kDialed:
    return "dialed";
  }
  return "accepted";
}

const char* statusName(moq::peering::PeerLinkStatus status) {
  switch (status) {
  case moq::peering::PeerLinkStatus::kConnecting:
    return "connecting";
  case moq::peering::PeerLinkStatus::kConnected:
    return "connected";
  case moq::peering::PeerLinkStatus::kDisconnected:
    return "disconnected";
  }
  return "connecting";
}

PeerInfo toPeerInfo(const moq::peering::PeerSnapshot& peer) {
  PeerInfo info;
  info.sessionId = peer.session_id;
  info.origin = originName(peer.origin);
  info.status = statusName(peer.status);
  info.host = peer.host;
  info.port = peer.port;
  info.identified = peer.identified;
  if (peer.identified) {
    info.nodeId = moq::peering::FormatNodeId(peer.node_id);
  }
  info.contact = peer.contact;
  info.nodeType = nodeTypeName(peer.node_type);
  info.srttUs = peer.srtt_us;
  info.metric = peer.metric;
  info.nodeMetric = peer.node_metric;
  info.connectTimeUs = peer.connect_time_us;
  info.subscribeTracks = peer.subscribe_tracks;
  info.publishes = peer.publishes;
  info.activeStreams = peer.active_streams;
  return info;
}

PeerCommandResult toCommand(const moq::peering::PeerAdminResult& result) {
  PeerCommandResult out;
  out.ok = result.ok;
  out.host = result.host;
  out.port = result.port;
  out.metric = result.metric;
  out.action = result.action;
  out.message = result.message;
  return out;
}

// A peer name that is larger than this is dropped rather than copied into relay state.
constexpr size_t kMaxNameElements = 64;
constexpr size_t kMaxNameBytes = 4096;
// Version-1 object header is 32 bytes. The payload cap is 1MiB.
constexpr size_t kMaxObjectBytes = (1u << 20) + 4096;

std::optional<std::vector<std::string>>
namespaceFromBytes(const std::vector<std::vector<uint8_t>>& raw) {
  if (raw.size() > kMaxNameElements) {
    return std::nullopt;
  }
  std::vector<std::string> out;
  out.reserve(raw.size());
  for (const auto& part : raw) {
    if (part.size() > kMaxNameBytes) {
      return std::nullopt;
    }
    out.emplace_back(reinterpret_cast<const char*>(part.data()), part.size());
  }
  return out;
}

std::optional<moq::peering::TrackName>
trackFromStrings(const std::vector<std::string>& nameSpace, const std::string& track) {
  if (nameSpace.size() > kMaxNameElements || track.size() > kMaxNameBytes) {
    return std::nullopt;
  }
  moq::peering::TrackName name;
  name.name_space.reserve(nameSpace.size());
  for (const auto& part : nameSpace) {
    if (part.size() > kMaxNameBytes) {
      return std::nullopt;
    }
    name.name_space.emplace_back(part.begin(), part.end());
  }
  name.name.assign(track.begin(), track.end());
  return name;
}

} // namespace

struct RuntimeState;

class RelayDelegate : public moq::peering::Delegate {
public:
  explicit RelayDelegate(RuntimeState* state) : state_(state) {}

  void OnPeerUp(std::uint64_t sessionId) override {
    emitLog(PeeringLogLevel::Info, "moq-peering session up id=" + std::to_string(sessionId));
  }

  void OnPeerDown(std::uint64_t sessionId) override {
    emitLog(PeeringLogLevel::Info, "moq-peering session down id=" + std::to_string(sessionId));
  }

  void OnPeerNamespace(const std::vector<std::vector<uint8_t>>& nameSpace, bool withdrawn) override;

  void OnPeerTrack(const moq::peering::TrackName& name, bool withdrawn) override;

  void OnPeerSubscribe(
      const moq::peering::TrackName& name,
      std::uint64_t trackHash,
      const moq::peering::SubscribeParams& params
  ) override;

  void OnPeerUnsubscribe(std::uint64_t trackHash, bool subscribersRemain) override;

  void OnPeerData(
      std::uint64_t trackHash,
      bool isNewStream,
      std::optional<std::uint64_t> streamId,
      std::shared_ptr<const std::vector<uint8_t>> data
  ) override;

  void OnPeerStreamClosed(std::uint64_t trackHash, std::uint64_t streamId, bool reset) override;

private:
  RuntimeState* state_;
};

struct RuntimeState {
  config::MoqPeeringConfig config;
  std::atomic<PeeringEvents*> events{nullptr};
  RelayDelegate delegate;
  std::unique_ptr<moq::peering::PeerManager> manager;
  mutable std::mutex mu;

  explicit RuntimeState(config::MoqPeeringConfig cfg) : config(std::move(cfg)), delegate(this) {}
};

void RelayDelegate::OnPeerNamespace(
    const std::vector<std::vector<uint8_t>>& nameSpace,
    bool withdrawn
) {
  auto parsed = namespaceFromBytes(nameSpace);
  if (!parsed) {
    emitLog(PeeringLogLevel::Warning, "moq-peering dropped a namespace that exceeds name limits");
    return;
  }
  if (PeeringEvents* events = state_->events.load(std::memory_order_acquire)) {
    events->onNamespace(std::move(*parsed), withdrawn);
  }
}

void RelayDelegate::OnPeerTrack(const moq::peering::TrackName& name, bool withdrawn) {
  auto parsed = namespaceFromBytes(name.name_space);
  if (!parsed || name.name.size() > kMaxNameBytes) {
    emitLog(PeeringLogLevel::Warning, "moq-peering dropped a track that exceeds name limits");
    return;
  }
  if (PeeringEvents* events = state_->events.load(std::memory_order_acquire)) {
    events->onTrack(
        std::move(*parsed),
        std::string(reinterpret_cast<const char*>(name.name.data()), name.name.size()),
        withdrawn
    );
  }
}

void RelayDelegate::OnPeerSubscribe(
    const moq::peering::TrackName& name,
    std::uint64_t trackHash,
    const moq::peering::SubscribeParams& params
) {
  auto parsed = namespaceFromBytes(name.name_space);
  if (!parsed || name.name.size() > kMaxNameBytes) {
    emitLog(PeeringLogLevel::Warning, "moq-peering dropped a subscribe that exceeds name limits");
    return;
  }
  if (PeeringEvents* events = state_->events.load(std::memory_order_acquire)) {
    events->onSubscribe(
        std::move(*parsed),
        std::string(reinterpret_cast<const char*>(name.name.data()), name.name.size()),
        trackHash,
        params.priority,
        params.forward
    );
  }
}

void RelayDelegate::OnPeerUnsubscribe(std::uint64_t trackHash, bool subscribersRemain) {
  if (PeeringEvents* events = state_->events.load(std::memory_order_acquire)) {
    events->onUnsubscribe(trackHash, subscribersRemain);
  }
}

void RelayDelegate::OnPeerData(
    std::uint64_t trackHash,
    bool isNewStream,
    std::optional<std::uint64_t> streamId,
    std::shared_ptr<const std::vector<uint8_t>> data
) {
  if (!data || data->empty()) {
    return;
  }
  if (data->size() > kMaxObjectBytes) {
    emitLog(PeeringLogLevel::Warning, "moq-peering dropped an object that exceeds the size limit");
    return;
  }
  if (PeeringEvents* events = state_->events.load(std::memory_order_acquire)) {
    events->onData(
        trackHash,
        isNewStream,
        streamId,
        std::vector<uint8_t>(data->begin(), data->end())
    );
  }
}

void RelayDelegate::OnPeerStreamClosed(
    std::uint64_t trackHash,
    std::uint64_t streamId,
    bool reset
) {
  if (PeeringEvents* events = state_->events.load(std::memory_order_acquire)) {
    events->onStreamClosed(trackHash, streamId, reset);
  }
}

void announce(
    RuntimeState& state,
    const std::vector<std::string>& nameSpace,
    const std::string& track,
    moq::peering::Announcement kind,
    bool withdraw
) {
  auto name = trackFromStrings(nameSpace, track);
  if (!name) {
    emitLog(PeeringLogLevel::Warning, "moq-peering dropped a local name that exceeds name limits");
    return;
  }
  std::lock_guard<std::mutex> lock(state.mu);
  if (!state.manager) {
    return;
  }
  // The delegate must not call back into the runtime. It only schedules relay work.
  state.manager->Announce(*name, kind, withdraw);
}

struct MoqPeeringRuntime::Impl {
  RuntimeState state;

  explicit Impl(config::MoqPeeringConfig cfg) : state(std::move(cfg)) {}

  ~Impl() {
    state.events.store(nullptr, std::memory_order_release);
    std::lock_guard<std::mutex> lock(state.mu);
    if (state.manager) {
      // In-flight callbacks may still finish. The manager joins its threads
      // before the delegate, which is a member destroyed after this body.
      state.manager->SetDelegate(nullptr);
      state.manager.reset();
    }
  }
};

void MoqPeeringRuntime::setLogger(PeeringLogFn fn) {
  gLogger.store(fn, std::memory_order_release);
}

MoqPeeringRuntime::MoqPeeringRuntime(const config::MoqPeeringConfig& config)
    : impl_(std::make_unique<Impl>(config)) {
  moq::peering::Config library;
  library.node_id = config.nodeId;
  library.node_type = toLibrary(config.nodeType);
  library.node_metric = config.nodeMetric;
  library.listen_port = config.listenPort;
  library.peers.reserve(config.peers.size());
  for (const auto& peer : config.peers) {
    library.peers.emplace_back(peer.host, peer.port);
  }
  library.tls_cert_filename = config.tls.certFile;
  library.tls_key_filename = config.tls.keyFile;
  library.object_ttl_ms = config.objectTtlMs;
  library.check_interval_ms = config.checkIntervalMs;
  library.init_queue_size = config.initQueueSize;
  library.debug = config.debug;

  impl_->state.manager = std::make_unique<moq::peering::PeerManager>(library);
  impl_->state.manager->SetDelegate(&impl_->state.delegate);

  for (const auto& peer : config.peers) {
    if (!peer.metric) {
      continue;
    }
    auto result = impl_->state.manager->SetPeerMetric(peer.host, peer.port, *peer.metric);
    if (!result.ok) {
      emitLog(
          PeeringLogLevel::Warning,
          "moq-peering failed to set metric for " + peer.host + ":" + std::to_string(peer.port) +
              ": " + result.message
      );
    }
  }

  emitLog(
      PeeringLogLevel::Info,
      "moq-peering listening on UDP " + std::to_string(config.listenPort) +
          " contact=" + config.nodeId +
          " node_id=" + moq::peering::FormatNodeId(impl_->state.manager->LocalNodeId()) +
          " type=" + nodeTypeName(config.nodeType)
  );
}

MoqPeeringRuntime::~MoqPeeringRuntime() = default;

void MoqPeeringRuntime::setEvents(PeeringEvents* events) {
  impl_->state.events.store(events, std::memory_order_release);
}

void MoqPeeringRuntime::announceNamespace(const std::vector<std::string>& nameSpace) {
  announce(impl_->state, nameSpace, "", moq::peering::Announcement::kNamespace, false);
}

void MoqPeeringRuntime::withdrawNamespace(const std::vector<std::string>& nameSpace) {
  announce(impl_->state, nameSpace, "", moq::peering::Announcement::kNamespace, true);
}

void MoqPeeringRuntime::announceTrack(
    const std::vector<std::string>& nameSpace,
    const std::string& track
) {
  announce(impl_->state, nameSpace, track, moq::peering::Announcement::kTrack, false);
}

void MoqPeeringRuntime::withdrawTrack(
    const std::vector<std::string>& nameSpace,
    const std::string& track
) {
  announce(impl_->state, nameSpace, track, moq::peering::Announcement::kTrack, true);
}

void MoqPeeringRuntime::subscribe(
    const std::vector<std::string>& nameSpace,
    const std::string& track,
    uint8_t priority,
    bool forward
) {
  auto name = trackFromStrings(nameSpace, track);
  if (!name) {
    emitLog(PeeringLogLevel::Warning, "moq-peering dropped a subscribe that exceeds name limits");
    return;
  }
  moq::peering::SubscribeParams params;
  params.priority = priority;
  params.forward = forward ? 1 : 0;
  params.delivery_timeout_ms = impl_->state.config.objectTtlMs;
  params.expires_ms = impl_->state.config.objectTtlMs;
  std::lock_guard<std::mutex> lock(impl_->state.mu);
  if (!impl_->state.manager) {
    return;
  }
  impl_->state.manager->Subscribe(*name, params);
}

void MoqPeeringRuntime::unsubscribe(
    const std::vector<std::string>& nameSpace,
    const std::string& track
) {
  auto name = trackFromStrings(nameSpace, track);
  if (!name) {
    return;
  }
  auto hash = moq::peering::PeerManager::HashTrack(*name);
  std::lock_guard<std::mutex> lock(impl_->state.mu);
  if (!impl_->state.manager) {
    return;
  }
  impl_->state.manager->Unsubscribe(hash);
}

void MoqPeeringRuntime::sendData(
    const std::vector<std::string>& nameSpace,
    const std::string& track,
    uint8_t priority,
    WireKind kind,
    uint64_t group,
    uint64_t subgroup,
    std::shared_ptr<const std::vector<uint8_t>> bytes
) {
  if (!bytes || bytes->empty()) {
    return;
  }
  if (bytes->size() > kMaxObjectBytes) {
    emitLog(
        PeeringLogLevel::Warning,
        "moq-peering dropped an outbound object that exceeds the size limit"
    );
    return;
  }
  auto name = trackFromStrings(nameSpace, track);
  if (!name) {
    emitLog(
        PeeringLogLevel::Warning,
        "moq-peering dropped an object whose name exceeds name limits"
    );
    return;
  }
  auto hash = moq::peering::PeerManager::HashTrack(*name);
  moq::peering::DataType type = moq::peering::DataType::kExistingStream;
  if (kind == WireKind::Datagram) {
    type = moq::peering::DataType::kDatagram;
  } else if (kind == WireKind::NewStream) {
    type = moq::peering::DataType::kNewStream;
  }
  std::lock_guard<std::mutex> lock(impl_->state.mu);
  if (!impl_->state.manager) {
    return;
  }
  impl_->state.manager->SendData(
      hash,
      priority,
      impl_->state.config.objectTtlMs,
      type,
      group,
      subgroup,
      std::move(bytes)
  );
}

void MoqPeeringRuntime::endSubgroup(
    const std::vector<std::string>& nameSpace,
    const std::string& track,
    uint64_t group,
    uint64_t subgroup,
    bool reset
) {
  auto name = trackFromStrings(nameSpace, track);
  if (!name) {
    return;
  }
  auto hash = moq::peering::PeerManager::HashTrack(*name);
  std::lock_guard<std::mutex> lock(impl_->state.mu);
  if (!impl_->state.manager) {
    return;
  }
  impl_->state.manager->EndSubgroup(hash, group, subgroup, reset);
}

std::optional<uint64_t> MoqPeeringRuntime::hashTrack(
    const std::vector<std::string>& nameSpace,
    const std::string& track
) const {
  auto name = trackFromStrings(nameSpace, track);
  if (!name) {
    return std::nullopt;
  }
  return moq::peering::PeerManager::HashTrack(*name);
}

PeeringStatus MoqPeeringRuntime::status() const {
  std::lock_guard<std::mutex> lock(impl_->state.mu);
  PeeringStatus out;
  out.contact = impl_->state.config.nodeId;
  out.nodeType = nodeTypeName(impl_->state.config.nodeType);
  out.nodeMetric = impl_->state.config.nodeMetric;
  out.listenPort = impl_->state.config.listenPort;
  if (!impl_->state.manager) {
    return out;
  }
  out.nodeId = moq::peering::FormatNodeId(impl_->state.manager->LocalNodeId());
  auto peers = impl_->state.manager->Peers();
  out.peers.reserve(peers.size());
  for (const auto& peer : peers) {
    out.peers.push_back(toPeerInfo(peer));
  }
  return out;
}

PeerCommandResult
MoqPeeringRuntime::addPeer(const std::string& host, uint16_t port, std::optional<uint64_t> metric) {
  std::lock_guard<std::mutex> lock(impl_->state.mu);
  if (!impl_->state.manager) {
    return PeerCommandResult{.ok = false, .message = "moq-peering is not running"};
  }
  return toCommand(impl_->state.manager->AddPeer(host, port, metric));
}

PeerCommandResult MoqPeeringRuntime::removePeer(const std::string& host, uint16_t port) {
  std::lock_guard<std::mutex> lock(impl_->state.mu);
  if (!impl_->state.manager) {
    return PeerCommandResult{.ok = false, .message = "moq-peering is not running"};
  }
  return toCommand(impl_->state.manager->RemovePeer(host, port));
}

PeerCommandResult MoqPeeringRuntime::restartPeer(const std::string& host, uint16_t port) {
  std::lock_guard<std::mutex> lock(impl_->state.mu);
  if (!impl_->state.manager) {
    return PeerCommandResult{.ok = false, .message = "moq-peering is not running"};
  }
  return toCommand(impl_->state.manager->RestartPeer(host, port));
}

PeerCommandResult
MoqPeeringRuntime::setPeerMetric(const std::string& host, uint16_t port, uint64_t metric) {
  std::lock_guard<std::mutex> lock(impl_->state.mu);
  if (!impl_->state.manager) {
    return PeerCommandResult{.ok = false, .message = "moq-peering is not running"};
  }
  return toCommand(impl_->state.manager->SetPeerMetric(host, port, metric));
}

} // namespace openmoq::moqx::peering
