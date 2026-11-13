/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "peering/MoqPeeringRuntime.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace openmoq::moqx::peering {

// JSON documents for the admin peering routes. Values that do not fit in a
// signed 64-bit integer are written as decimal strings so they are not truncated.
std::string peeringStatusJson(const PeeringStatus& status);
std::string peerCommandJson(const PeerCommandResult& result);
std::string clusterPeeringJson(bool enabled, std::optional<uint64_t> hopId);
std::string peeringErrorJson(std::string_view error);

} // namespace openmoq::moqx::peering
