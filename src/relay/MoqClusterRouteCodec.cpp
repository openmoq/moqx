/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/MoqClusterRouteCodec.h"

#include "relay/ClusterPath.h"

#include <folly/Utility.h>
#include <folly/logging/xlog.h>
#include <moxygen/MoQFramer.h>

using namespace moxygen;

namespace openmoq::moqx {

std::optional<IngestedRoute> MoqClusterRouteCodec::readRoute(
    const TrackRequestParameters& params,
    const ParticipantInfo& publisher
) const {
  auto update = readRouteUpdate(params, publisher);
  if (!update) {
    return std::nullopt;
  }
  XCHECK(!update->path.empty()) << "moxygen requires HOP_PATH from a MoQ Cluster session";
  return IngestedRoute{
      .path = std::move(update->path),
      .cost = update->cost.value_or(publisher.routing.linkCost),
  };
}

std::optional<RouteParams> MoqClusterRouteCodec::readRouteUpdate(
    const TrackRequestParameters& params,
    const ParticipantInfo& publisher
) const {
  RouteParams out;
  if (const auto* hopPath = params.getFirstParam(TrackRequestParamKey::HOP_PATH)) {
    auto decoded = decodeRelayHopPath(hopPath->asString, publisher.version);
    XCHECK(decoded.hasValue()) << "moxygen rejects a malformed HOP_PATH";
    if (clusterPathContains(decoded.value(), localHopID_)) {
      return std::nullopt;
    }
    out.path = std::move(decoded.value());
  }
  if (const auto* cost = params.getFirstParam(TrackRequestParamKey::ROUTE_COST)) {
    out.cost = clusterAddCost(cost->asUint64, publisher.routing.linkCost);
  }
  return out;
}

void MoqClusterRouteCodec::writeRoute(
    TrackRequestParameters& params,
    const ParticipantInfo& subscriber,
    const std::vector<uint64_t>& path,
    uint64_t cost
) const {
  params.eraseAllParamsOfType(TrackRequestParamKey::HOP_PATH);
  params.eraseAllParamsOfType(TrackRequestParamKey::ROUTE_COST);
  auto outgoingPath = path;
  outgoingPath.push_back(localHopID_);
  auto encodedPath = encodeRelayHopPath(outgoingPath, subscriber.version);
  XCHECK(encodedPath.hasValue());
  params.insertParam(Parameter(
      folly::to_underlying(TrackRequestParamKey::HOP_PATH),
      std::move(encodedPath.value())
  ));
  params.insertParam(Parameter(folly::to_underlying(TrackRequestParamKey::ROUTE_COST), cost));
}

} // namespace openmoq::moqx
