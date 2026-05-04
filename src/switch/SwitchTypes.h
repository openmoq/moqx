/*
 * Copyright (c) Synamedia
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once
#include <cstdint>

namespace openmoq::moqx {

struct SwitchTransition {
  uint64_t switchingGroupID;
  uint64_t liveEdgeGroupID;
};

// Private-use parameter key for SWITCH_TRANSITION (TBD at IANA registration).
constexpr uint64_t kSwitchTransitionParamKey = 0xFF01;

} // namespace openmoq::moqx
