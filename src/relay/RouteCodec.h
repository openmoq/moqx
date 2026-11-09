/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "relay/Participant.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace openmoq::moqx {

// A route that the relay learned from one announcement, in the relay's route model: the
// relays that it passed through, and a cost.
struct IngestedRoute {
  // The relays that the announcement passed through, nearest last. A sender that does not
  // carry a path gets {kMoQClusterAnonHopId}.
  std::vector<uint64_t> path;
  // The sender's advertised cost plus its link cost. Lower wins.
  uint64_t cost;
};

// The path and cost that a route update carries. Each field left empty keeps its value.
struct RouteParams {
  std::vector<uint64_t> path;
  std::optional<uint64_t> cost; // with the link cost added
};

// Reads and writes the route parameters on one participant's announcements, translating
// between its wire format and the relay's route model. Each participant's routing holds
// one. The relay calls the publisher's codec for announcements that it receives, and the
// subscriber's codec for announcements that it sends. A codec does not hold state, so any
// executor may call it, and one instance may serve many participants.
class RouteCodec {
public:
  virtual ~RouteCodec() = default;

  // Reads the route from a PUBLISH_NAMESPACE or PUBLISH that publisher sent. Returns unset
  // when the route passes through this relay, and the relay then drops the announcement
  // as a loop.
  virtual std::optional<IngestedRoute> readRoute(
      const moxygen::TrackRequestParameters& params,
      const ParticipantInfo& publisher
  ) const = 0;

  // Reads a route change from a REQUEST_UPDATE that publisher sent for an earlier
  // announcement. Returns unset when the new route passes through this relay, and the
  // relay then withdraws the announcement. A codec whose participants do not change
  // routes returns an empty RouteParams.
  virtual std::optional<RouteParams> readRouteUpdate(
      const moxygen::TrackRequestParameters& params,
      const ParticipantInfo& publisher
  ) const = 0;

  // Writes the route for an announcement that the relay sends to subscriber, replacing any
  // route parameters already in params. path and cost describe the best route to the
  // publisher. The codec adds this relay where its format requires it.
  virtual void writeRoute(
      moxygen::TrackRequestParameters& params,
      const ParticipantInfo& subscriber,
      const std::vector<uint64_t>& path,
      uint64_t cost
  ) const = 0;
};

} // namespace openmoq::moqx
