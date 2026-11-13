/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Resolved moq-peering settings. Field names follow moq::peering::Config.
// Certificate and key values are filesystem paths. The private key is never
// copied into this struct.
namespace openmoq::moqx::config {

enum class MoqPeeringNodeType { Via, Edge, Stub };

struct MoqPeeringTlsConfig {
  std::string certFile;
  std::string keyFile;
};

struct MoqPeeringPeer {
  std::string host;
  uint16_t port{0};
  std::optional<uint64_t> metric;
};

struct MoqPeeringConfig {
  std::string nodeId;
  MoqPeeringNodeType nodeType{MoqPeeringNodeType::Edge};
  uint64_t nodeMetric{100};
  uint16_t listenPort{33434};
  std::vector<MoqPeeringPeer> peers;
  MoqPeeringTlsConfig tls;
  uint32_t objectTtlMs{5000};
  uint64_t checkIntervalMs{5000};
  uint32_t initQueueSize{5000};
  bool debug{false};
};

} // namespace openmoq::moqx::config
