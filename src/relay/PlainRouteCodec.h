/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "relay/RouteCodec.h"

#include <moxygen/MoQFramer.h>

#include <memory>

namespace openmoq::moqx {

// The codec of a participant that does not carry routes: a client, or a MoQ session that did
// not negotiate MoQ Cluster. Its announcements are one anonymous hop at its link cost, and it
// does not receive route parameters.
class PlainRouteCodec : public RouteCodec {
public:
  std::optional<IngestedRoute> readRoute(
      const moxygen::TrackRequestParameters& /*params*/,
      const ParticipantInfo& publisher
  ) const override {
    return IngestedRoute{
        .path = {moxygen::kMoQClusterAnonHopId},
        .cost = publisher.routing.linkCost,
    };
  }

  // The route stays as first announced.
  std::optional<RouteParams> readRouteUpdate(
      const moxygen::TrackRequestParameters& /*params*/,
      const ParticipantInfo& /*publisher*/
  ) const override {
    return RouteParams{};
  }

  void writeRoute(
      moxygen::TrackRequestParameters& params,
      const ParticipantInfo& /*subscriber*/,
      const std::vector<uint64_t>& /*path*/,
      uint64_t /*cost*/
  ) const override {
    params.eraseAllParamsOfType(moxygen::MoQClusterExtension::kHopPathParam);
    params.eraseAllParamsOfType(moxygen::MoQClusterExtension::kRouteCostParam);
  }
};

// Returns setup with the plain codec, without route updates or an exclusion. The codec is
// shared, since it does not hold state.
inline ParticipantRouting plainRouting(ParticipantRouting setup) {
  static const auto codec = std::make_shared<const PlainRouteCodec>();
  setup.codec = codec;
  setup.fromPeering = false;
  setup.wantsUpdates = false;
  setup.exclusion.reset();
  return setup;
}

} // namespace openmoq::moqx
