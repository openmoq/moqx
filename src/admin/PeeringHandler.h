/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "config/Config.h"

namespace openmoq::moqx {
class MoqxRelayContext;
}

namespace openmoq::moqx::admin {

class AdminServer;

// Registers the peering admin routes.
//
// GET  /peering
//   cluster:     {"method":"cluster","enabled":bool,"hop_id":n}
//   moq-peering: method, local node id, and each session's state and metrics
//
// POST /peering/add-peer        {"host","port","metric"?}
// POST /peering/remove-peer     {"host","port"}
// POST /peering/restart-peer    {"host","port"}
// POST /peering/set-peer-metric {"host","port","metric"}
//
// context is null only when the caller has no relay. POST routes answer 409
// when the process started with the cluster method. The admin listener is the
// existing management port; add-peer dials the named host, so that port stays
// on a trusted network.
void registerPeeringRoutes(
    AdminServer& adminServer,
    config::PeeringMethod method,
    std::optional<uint64_t> clusterHopId,
    std::shared_ptr<MoqxRelayContext> context
);

} // namespace openmoq::moqx::admin
