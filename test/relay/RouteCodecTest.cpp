/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/MoqClusterRouteCodec.h"
#include "relay/PlainRouteCodec.h"

#include <folly/portability/GTest.h>
#include <moxygen/MoQVersions.h>
#include <moxygen/openmoq/extensions/MoQClusterExtension.h>

#include <algorithm>
#include <limits>

using namespace moxygen;
using namespace openmoq::moqx;

namespace {

constexpr uint64_t kLocalHop = 5;

ParticipantInfo clusterPeer(uint64_t peerHop, uint64_t linkCost = 2) {
  return ParticipantInfo{
      .sessionId = SessionId(7),
      .version = kVersionDraft18,
      .routing = {.relayHops = true, .peerHopID = peerHop, .linkCost = linkCost},
  };
}

TrackRequestParameters clusterParams(const std::vector<uint64_t>& path, uint64_t cost) {
  TrackRequestParameters params(FrameType::PUBLISH_NAMESPACE);
  params.insertParam(Parameter(
      MoQClusterExtension::kHopPathParam,
      encodeRelayHopPath(path, kVersionDraft18).value()
  ));
  params.insertParam(Parameter(MoQClusterExtension::kRouteCostParam, cost));
  return params;
}

TEST(MoqClusterRouteCodecTest, ClusterPublisherAddsItsLinkCost) {
  MoqClusterRouteCodec codec(kLocalHop);
  auto route = codec.readRoute(clusterParams({3, 4}, 10), clusterPeer(4, /*linkCost=*/2));
  ASSERT_TRUE(route.has_value());
  EXPECT_EQ(route->path, (std::vector<uint64_t>{3, 4}));
  EXPECT_EQ(route->cost, 12u);
}

TEST(MoqClusterRouteCodecTest, RouteCostSaturates) {
  MoqClusterRouteCodec codec(kLocalHop);
  constexpr auto kMax = std::numeric_limits<uint64_t>::max();
  auto route = codec.readRoute(clusterParams({3}, kMax - 1), clusterPeer(3, /*linkCost=*/2));
  ASSERT_TRUE(route.has_value());
  EXPECT_EQ(route->cost, kMax);
}

TEST(MoqClusterRouteCodecTest, MissingRouteCostFallsBackToLinkCost) {
  MoqClusterRouteCodec codec(kLocalHop);
  TrackRequestParameters params(FrameType::PUBLISH_NAMESPACE);
  params.insertParam(Parameter(
      MoQClusterExtension::kHopPathParam,
      encodeRelayHopPath({3}, kVersionDraft18).value()
  ));
  auto route = codec.readRoute(params, clusterPeer(3, /*linkCost=*/4));
  ASSERT_TRUE(route.has_value());
  EXPECT_EQ(route->path, std::vector<uint64_t>{3});
  EXPECT_EQ(route->cost, 4u);
}

TEST(MoqClusterRouteCodecTest, PathThroughThisRelayIsDropped) {
  MoqClusterRouteCodec codec(kLocalHop);
  EXPECT_FALSE(codec.readRoute(clusterParams({3, kLocalHop}, 10), clusterPeer(3)).has_value());
  EXPECT_FALSE(codec.readRouteUpdate(clusterParams({kLocalHop}, 10), clusterPeer(3)).has_value());
}

TEST(MoqClusterRouteCodecTest, UpdateWithoutParametersKeepsTheRoute) {
  MoqClusterRouteCodec codec(kLocalHop);
  auto update =
      codec.readRouteUpdate(TrackRequestParameters(FrameType::REQUEST_UPDATE), clusterPeer(3));
  ASSERT_TRUE(update.has_value());
  EXPECT_TRUE(update->path.empty());
  EXPECT_FALSE(update->cost.has_value());
}

TEST(MoqClusterRouteCodecTest, WriteAppendsThisRelay) {
  MoqClusterRouteCodec codec(kLocalHop);
  TrackRequestParameters params(FrameType::PUBLISH_NAMESPACE);
  // Stale values from an earlier write, which writeRoute replaces.
  params.insertParam(Parameter(
      MoQClusterExtension::kHopPathParam,
      encodeRelayHopPath({5}, kVersionDraft18).value()
  ));
  params.insertParam(Parameter(MoQClusterExtension::kRouteCostParam, uint64_t{99}));
  codec.writeRoute(params, clusterPeer(9), {3}, 12);
  auto countOf = [&](uint64_t key) {
    return std::count_if(params.begin(), params.end(), [&](const Parameter& p) {
      return p.key == key;
    });
  };
  EXPECT_EQ(countOf(MoQClusterExtension::kHopPathParam), 1);
  EXPECT_EQ(countOf(MoQClusterExtension::kRouteCostParam), 1);
  const auto* hopPath = params.getFirstParam(MoQClusterExtension::kHopPathParam);
  ASSERT_NE(hopPath, nullptr);
  EXPECT_EQ(
      decodeRelayHopPath(hopPath->asString, kVersionDraft18).value(),
      (std::vector<uint64_t>{3, kLocalHop})
  );
  const auto* cost = params.getFirstParam(MoQClusterExtension::kRouteCostParam);
  ASSERT_NE(cost, nullptr);
  EXPECT_EQ(cost->asUint64, 12u);
}

TEST(PlainRouteCodecTest, IgnoresClusterParameters) {
  PlainRouteCodec codec;
  auto route = codec.readRoute(clusterParams({3, kLocalHop}, 10), clusterPeer(3, /*linkCost=*/2));
  ASSERT_TRUE(route.has_value());
  EXPECT_EQ(route->path, std::vector<uint64_t>{kMoQClusterAnonHopId});
  EXPECT_EQ(route->cost, 2u);
  auto update = codec.readRouteUpdate(clusterParams({kLocalHop}, 10), clusterPeer(3));
  ASSERT_TRUE(update.has_value());
  EXPECT_TRUE(update->path.empty());
}

TEST(PlainRouteCodecTest, WriteStripsParameters) {
  PlainRouteCodec codec;
  auto params = clusterParams({3}, 12);
  codec.writeRoute(params, clusterPeer(9), {3}, 12);
  EXPECT_EQ(params.getFirstParam(MoQClusterExtension::kHopPathParam), nullptr);
  EXPECT_EQ(params.getFirstParam(MoQClusterExtension::kRouteCostParam), nullptr);
}

TEST(PlainRouteCodecTest, PlainRoutingKeepsSetupAndClearsPolicy) {
  ParticipantRouting setup{
      .relayHops = true,
      .peerHopID = 9,
      .linkCost = 4,
      .fromPeering = true,
      .wantsUpdates = true,
      .exclusion = RouteExclusion{.session = SessionId(7), .hop = 9},
  };
  auto routing = plainRouting(setup);
  EXPECT_TRUE(routing.relayHops);
  EXPECT_EQ(routing.peerHopID, 9u);
  EXPECT_EQ(routing.linkCost, 4u);
  ASSERT_NE(routing.codec, nullptr);
  EXPECT_FALSE(routing.fromPeering);
  EXPECT_FALSE(routing.wantsUpdates);
  EXPECT_FALSE(routing.exclusion.has_value());
}

} // namespace
