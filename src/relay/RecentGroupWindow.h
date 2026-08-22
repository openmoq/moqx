/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace openmoq::moqx {

// A most-recently-used window of group IDs, so counting distinct groups
// tolerates the subgroups of several groups arriving interleaved. Bounds how
// deep an interleave can be before a group is counted twice.
//
// LRU rather than insertion order: a group that keeps receiving subgroups stays
// in the window however many other groups open alongside it.
class RecentGroupWindow {
public:
  static constexpr size_t kSize = 3;

  // True the first time a group is seen, or the first time since it aged out.
  [[nodiscard]] bool admit(uint64_t groupID) {
    for (size_t i = 0; i < count_; ++i) {
      if (groups_[i] == groupID) {
        std::rotate(groups_.begin(), groups_.begin() + i, groups_.begin() + i + 1);
        return false;
      }
    }
    count_ = std::min(count_ + 1, kSize);
    std::rotate(groups_.begin(), groups_.end() - 1, groups_.end());
    groups_[0] = groupID;
    return true;
  }

private:
  // Lookup and the LRU update are both linear scans on the data path, so this
  // stays an array rather than a map only while the window is tiny.
  static_assert(kSize <= 8, "widening past this wants folly::findFixed (or a map), not a scan");

  std::array<uint64_t, kSize> groups_{};
  size_t count_{0};
};

} // namespace openmoq::moqx
