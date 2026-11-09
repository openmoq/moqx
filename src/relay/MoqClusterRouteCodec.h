/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "relay/RouteCodec.h"

namespace openmoq::moqx {

// The codec of a MoQ session that negotiated MoQ Cluster (draft-lcurley-moq-cluster). Routes
// carry HOP_PATH and ROUTE_COST, and a route that passes through this relay is a loop.
class MoqClusterRouteCodec : public RouteCodec {
public:
  // localHopID is this relay's MoQ Cluster Hop ID.
  explicit MoqClusterRouteCodec(uint64_t localHopID) : localHopID_(localHopID) {}

  std::optional<IngestedRoute> readRoute(
      const moxygen::TrackRequestParameters& params,
      const ParticipantInfo& publisher
  ) const override;
  std::optional<RouteParams> readRouteUpdate(
      const moxygen::TrackRequestParameters& params,
      const ParticipantInfo& publisher
  ) const override;
  void writeRoute(
      moxygen::TrackRequestParameters& params,
      const ParticipantInfo& subscriber,
      const std::vector<uint64_t>& path,
      uint64_t cost
  ) const override;

private:
  uint64_t localHopID_;
};

} // namespace openmoq::moqx
