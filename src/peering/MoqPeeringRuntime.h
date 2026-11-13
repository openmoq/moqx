/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "config/MoqPeeringConfig.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Lives in libmoqx_peering so moq-peering's picoquic stays out of the relay
// binary, which already links moxygen's picoquic. Log lines are handed to a
// callback in the main executable so they use that process's logger.

#if defined(__GNUC__)
#define MOQX_PEERING_EXPORT __attribute__((visibility("default")))
#else
#define MOQX_PEERING_EXPORT
#endif

namespace openmoq::moqx::peering {

enum class PeeringLogLevel { Info, Warning, Error };

using PeeringLogFn = void (*)(PeeringLogLevel level, const char* message);

struct PeerInfo {
  uint64_t sessionId{0};
  std::string origin;
  std::string status;
  std::string host;
  uint16_t port{0};
  bool identified{false};
  std::string nodeId;
  std::string contact;
  std::string nodeType;
  uint64_t srttUs{0};
  uint64_t metric{0};
  uint64_t nodeMetric{0};
  uint64_t connectTimeUs{0};
  uint64_t subscribeTracks{0};
  uint64_t publishes{0};
  uint64_t activeStreams{0};
};

struct PeeringStatus {
  std::string nodeId;
  std::string contact;
  std::string nodeType;
  uint64_t nodeMetric{0};
  uint16_t listenPort{0};
  std::vector<PeerInfo> peers;
};

struct PeerCommandResult {
  bool ok{false};
  std::string host;
  uint16_t port{0};
  std::optional<uint64_t> metric;
  std::string action;
  std::string message;
};

// Callbacks from moq-peering. They run on peering threads and must return
// without calling back into MoqPeeringRuntime.
class MOQX_PEERING_EXPORT PeeringEvents {
public:
  virtual ~PeeringEvents() = default;
  virtual void onNamespace(std::vector<std::string> nameSpace, bool withdrawn) = 0;
  virtual void onTrack(std::vector<std::string> nameSpace, std::string track, bool withdrawn) = 0;

  // A peer subscribed to a local name. forward is 0 when the peer asked not
  // to receive objects; the relay still resumes a paused local publisher.
  // trackHash is the value later passed to onUnsubscribe and onData.
  virtual void onSubscribe(
      std::vector<std::string> /*nameSpace*/,
      std::string /*track*/,
      uint64_t /*trackHash*/,
      uint8_t /*priority*/,
      uint8_t /*forward*/
  ) {}
  virtual void onUnsubscribe(uint64_t /*trackHash*/, bool /*subscribersRemain*/) {}

  // MoQT bytes for a track this node subscribed to. A datagram has no stream
  // id. A new stream begins with the subgroup header. Later chunks on that
  // stream are the following objects.
  virtual void onData(
      uint64_t /*trackHash*/,
      bool /*isNewStream*/,
      std::optional<uint64_t> /*streamId*/,
      std::vector<uint8_t> /*bytes*/
  ) {}
  virtual void onStreamClosed(uint64_t /*trackHash*/, uint64_t /*streamId*/, bool /*reset*/) {}
};

class MOQX_PEERING_EXPORT MoqPeeringRuntime {
public:
  // Call once, before the first runtime is constructed. The function is invoked
  // from peering threads and must be safe to call concurrently.
  static void setLogger(PeeringLogFn fn);

  explicit MoqPeeringRuntime(const config::MoqPeeringConfig& config);
  ~MoqPeeringRuntime();

  MoqPeeringRuntime(const MoqPeeringRuntime&) = delete;
  MoqPeeringRuntime& operator=(const MoqPeeringRuntime&) = delete;

  // The pointer is not owned. Pass nullptr before destroying the events object,
  // and only after in-flight callbacks can no longer touch it. Callbacks that
  // already entered may still finish.
  void setEvents(PeeringEvents* events);

  // Announce or withdraw a name. A namespace uses an empty track. Names that
  // exceed the peering limits are dropped.
  void announceNamespace(const std::vector<std::string>& nameSpace);
  void withdrawNamespace(const std::vector<std::string>& nameSpace);
  void announceTrack(const std::vector<std::string>& nameSpace, const std::string& track);
  void withdrawTrack(const std::vector<std::string>& nameSpace, const std::string& track);

  // Ask peers for a track they announced. Delivery timeout and expiry use
  // object_ttl_ms. Names that exceed the peering limits are dropped.
  void subscribe(
      const std::vector<std::string>& nameSpace,
      const std::string& track,
      uint8_t priority,
      bool forward
  );
  void unsubscribe(const std::vector<std::string>& nameSpace, const std::string& track);

  enum class WireKind : uint8_t { NewStream, ExistingStream, Datagram };

  // MoQT bytes. A new stream starts with the subgroup header. An existing
  // stream continues that subgroup. A datagram is one MoQT datagram. Buffers
  // larger than 1MiB plus the header allowance are dropped.
  void sendData(
      const std::vector<std::string>& nameSpace,
      const std::string& track,
      uint8_t priority,
      WireKind kind,
      uint64_t group,
      uint64_t subgroup,
      std::shared_ptr<const std::vector<uint8_t>> bytes
  );
  void endSubgroup(
      const std::vector<std::string>& nameSpace,
      const std::string& track,
      uint64_t group,
      uint64_t subgroup,
      bool reset
  );

  // Full-track hash used by subscribe, unsubscribe, and object callbacks.
  // Empty when the name exceeds the peering limits.
  std::optional<uint64_t>
  hashTrack(const std::vector<std::string>& nameSpace, const std::string& track) const;

  PeeringStatus status() const;
  PeerCommandResult addPeer(const std::string& host, uint16_t port, std::optional<uint64_t> metric);
  PeerCommandResult removePeer(const std::string& host, uint16_t port);
  PeerCommandResult restartPeer(const std::string& host, uint16_t port);
  PeerCommandResult setPeerMetric(const std::string& host, uint16_t port, uint64_t metric);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace openmoq::moqx::peering
