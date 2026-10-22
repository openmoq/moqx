/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace openmoq::moqx {

// MoQ Cluster HOP_PATH rules. Hop ID 0 may repeat without forming a loop.

inline bool clusterPathContains(const std::vector<uint64_t>& path, uint64_t hop) {
  return hop != 0 && std::find(path.begin(), path.end(), hop) != path.end();
}

inline uint64_t clusterAddCost(uint64_t cost, uint64_t link) {
  constexpr auto max = std::numeric_limits<uint64_t>::max();
  return cost > max - link ? max : cost + link;
}

} // namespace openmoq::moqx
