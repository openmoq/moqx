/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "peering/PeerJson.h"

#include <limits>

#include <folly/json/dynamic.h>
#include <folly/json/json.h>

namespace openmoq::moqx::peering {
namespace {

void putUint(folly::dynamic& obj, const char* key, uint64_t value) {
  if (value <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    obj[key] = static_cast<int64_t>(value);
  } else {
    obj[key] = std::to_string(value);
  }
}

folly::dynamic peerDynamic(const PeerInfo& peer) {
  folly::dynamic obj = folly::dynamic::object();
  putUint(obj, "session_id", peer.sessionId);
  obj["origin"] = peer.origin;
  obj["status"] = peer.status;
  obj["host"] = peer.host;
  obj["port"] = peer.port;
  obj["identified"] = peer.identified;
  obj["node_id"] = peer.nodeId;
  obj["contact"] = peer.contact;
  obj["node_type"] = peer.nodeType;
  putUint(obj, "srtt_us", peer.srttUs);
  putUint(obj, "metric", peer.metric);
  putUint(obj, "node_metric", peer.nodeMetric);
  putUint(obj, "connect_time_us", peer.connectTimeUs);
  putUint(obj, "subscribe_tracks", peer.subscribeTracks);
  putUint(obj, "publishes", peer.publishes);
  putUint(obj, "active_streams", peer.activeStreams);
  return obj;
}

} // namespace

std::string peeringStatusJson(const PeeringStatus& status) {
  folly::dynamic obj = folly::dynamic::object();
  obj["method"] = "moq-peering";
  obj["node_id"] = status.nodeId;
  obj["contact"] = status.contact;
  obj["node_type"] = status.nodeType;
  putUint(obj, "node_metric", status.nodeMetric);
  obj["listen_port"] = status.listenPort;
  folly::dynamic peers = folly::dynamic::array();
  for (const auto& peer : status.peers) {
    peers.push_back(peerDynamic(peer));
  }
  obj["peers"] = std::move(peers);
  return folly::toJson(obj) + "\n";
}

std::string peerCommandJson(const PeerCommandResult& result) {
  folly::dynamic obj = folly::dynamic::object();
  obj["ok"] = result.ok;
  obj["host"] = result.host;
  obj["port"] = result.port;
  if (result.metric) {
    putUint(obj, "metric", *result.metric);
  } else {
    obj["metric"] = nullptr;
  }
  obj["action"] = result.action;
  obj["message"] = result.message;
  return folly::toJson(obj) + "\n";
}

std::string clusterPeeringJson(bool enabled, std::optional<uint64_t> hopId) {
  folly::dynamic obj = folly::dynamic::object();
  obj["method"] = "cluster";
  obj["enabled"] = enabled;
  if (hopId) {
    putUint(obj, "hop_id", *hopId);
  }
  return folly::toJson(obj) + "\n";
}

std::string peeringErrorJson(std::string_view error) {
  folly::dynamic obj = folly::dynamic::object();
  obj["error"] = std::string(error);
  return folly::toJson(obj) + "\n";
}

} // namespace openmoq::moqx::peering
