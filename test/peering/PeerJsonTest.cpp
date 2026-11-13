/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "peering/PeerJson.h"

#include <cstdint>
#include <limits>

#include <folly/json/json.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace openmoq::moqx::peering {
namespace {

using ::testing::HasSubstr;

TEST(PeerJson, StatusCarriesPeerStateAndMetrics) {
  PeeringStatus status;
  status.nodeId = "1.0:2.0";
  status.contact = "edge-sea-1";
  status.nodeType = "edge";
  status.nodeMetric = 100;
  status.listenPort = 33434;

  PeerInfo peer;
  peer.sessionId = 7;
  peer.origin = "dialed";
  peer.status = "connected";
  peer.host = "192.0.2.10";
  peer.port = 33440;
  peer.identified = true;
  peer.nodeId = "9.0:1.0";
  peer.contact = "edge-other";
  peer.nodeType = "via";
  peer.srttUs = 1500;
  peer.metric = 150;
  peer.nodeMetric = 150;
  peer.connectTimeUs = 10;
  peer.subscribeTracks = 2;
  peer.publishes = 3;
  peer.activeStreams = 4;
  status.peers.push_back(peer);

  const auto parsed = folly::parseJson(peeringStatusJson(status));
  EXPECT_EQ(parsed["method"].asString(), "moq-peering");
  EXPECT_EQ(parsed["node_id"].asString(), "1.0:2.0");
  EXPECT_EQ(parsed["contact"].asString(), "edge-sea-1");
  EXPECT_EQ(parsed["listen_port"].asInt(), 33434);
  ASSERT_EQ(parsed["peers"].size(), 1);
  const auto& item = parsed["peers"][0];
  EXPECT_EQ(item["origin"].asString(), "dialed");
  EXPECT_EQ(item["status"].asString(), "connected");
  EXPECT_EQ(item["host"].asString(), "192.0.2.10");
  EXPECT_EQ(item["port"].asInt(), 33440);
  EXPECT_TRUE(item["identified"].asBool());
  EXPECT_EQ(item["node_type"].asString(), "via");
  EXPECT_EQ(item["srtt_us"].asInt(), 1500);
  EXPECT_EQ(item["metric"].asInt(), 150);
  EXPECT_EQ(item["node_metric"].asInt(), 150);
  EXPECT_EQ(item["subscribe_tracks"].asInt(), 2);
  EXPECT_EQ(item["publishes"].asInt(), 3);
  EXPECT_EQ(item["active_streams"].asInt(), 4);
}

TEST(PeerJson, FailedCommandKeepsLibraryMessage) {
  PeerCommandResult result;
  result.ok = false;
  result.host = "192.0.2.10";
  result.port = 33440;
  result.message = "duplicate peer 192.0.2.10:33440";

  const auto parsed = folly::parseJson(peerCommandJson(result));
  EXPECT_FALSE(parsed["ok"].asBool());
  EXPECT_TRUE(parsed["metric"].isNull());
  EXPECT_EQ(parsed["action"].asString(), "");
  EXPECT_THAT(parsed["message"].asString(), HasSubstr("duplicate peer"));
}

TEST(PeerJson, MetricAboveInt64IsADecimalString) {
  PeerCommandResult result;
  result.ok = true;
  result.metric = (uint64_t{1} << 63) + 5;

  const auto parsed = folly::parseJson(peerCommandJson(result));
  EXPECT_TRUE(parsed["metric"].isString());
  EXPECT_EQ(parsed["metric"].asString(), "9223372036854775813");
}

TEST(PeerJson, ClusterDocumentOmitsHopIdWhenDisabled) {
  const auto disabled = folly::parseJson(clusterPeeringJson(false, std::nullopt));
  EXPECT_EQ(disabled["method"].asString(), "cluster");
  EXPECT_FALSE(disabled["enabled"].asBool());
  EXPECT_EQ(disabled.get_ptr("hop_id"), nullptr);

  const auto enabled = folly::parseJson(clusterPeeringJson(true, uint64_t{1001}));
  EXPECT_TRUE(enabled["enabled"].asBool());
  EXPECT_EQ(enabled["hop_id"].asInt(), 1001);
}

TEST(PeerJson, ErrorIsAnObject) {
  const auto parsed =
      folly::parseJson(peeringErrorJson("peer host contains a character that is not allowed"));
  EXPECT_THAT(parsed["error"].asString(), HasSubstr("not allowed"));
}

} // namespace
} // namespace openmoq::moqx::peering
