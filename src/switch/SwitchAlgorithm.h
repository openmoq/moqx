/*
 * Copyright (c) Synamedia
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <folly/container/F14Set.h>
#include <algorithm>
#include <cstdint>
#include <optional>

namespace openmoq::moqx {

// Returns the smallest g in [minimumGroupID, min(currentLarge, targetLarge)]
// such that, for every gp in [g, targetLarge), if gp <= currentLarge then gp
// is in availableTarget.
//
// Interpretation: g is a valid switch point when every group in the catch-up
// range [g, targetLarge) that the current track has already reached (gp <=
// currentLarge) is confirmed available on the target track. Groups beyond
// currentLarge do not yet need to be available (they will arrive live).
//
// Returns nullopt when minimumGroupID > min(currentLarge, targetLarge), i.e.
// neither track has reached the minimum yet.
inline std::optional<uint64_t> findGswitch(
    const folly::F14FastSet<uint64_t>& availableTarget,
    uint64_t currentLarge,
    uint64_t targetLarge,
    uint64_t minimumGroupID) {
  for (uint64_t g = minimumGroupID;
       g <= std::min(currentLarge, targetLarge);
       ++g) {
    bool ok = true;
    for (uint64_t gp = g; gp < targetLarge; ++gp) {
      if (gp <= currentLarge && !availableTarget.count(gp)) {
        ok = false;
        break;
      }
    }
    if (ok) {
      return g;
    }
  }
  return std::nullopt;
}

} // namespace openmoq::moqx
