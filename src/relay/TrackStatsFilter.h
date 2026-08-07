/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <array>
#include <cstddef>
#include <memory>

#include <moxygen/MoQFilters.h>

#include "stats/TrackStatsRegistry.h"

namespace openmoq::moqx {

// Counts objects flowing through one point of the data plane into the
// per-thread TrackStats for its track. Installed twice: once on the relay
// chain (ingest) and once per downstream subscriber (egress).
//
// Constructed on the thread that will run it — it caches the TrackStats
// pointer and never consults the collector again.
class TrackStatsFilter : public moxygen::TrackConsumerFilter,
                         public std::enable_shared_from_this<TrackStatsFilter> {
public:
  using Direction = stats::TrackDirection;

  TrackStatsFilter(
      std::shared_ptr<stats::TrackStats> stats,
      Direction direction,
      std::shared_ptr<moxygen::TrackConsumer> downstream
  );

  ~TrackStatsFilter() override;

  folly::Expected<std::shared_ptr<moxygen::SubgroupConsumer>, moxygen::MoQPublishError>
  beginSubgroup(
      uint64_t groupID,
      uint64_t subgroupID,
      moxygen::Priority priority,
      moxygen::BeginSubgroupOptions options = {}
  ) override;

  folly::Expected<folly::Unit, moxygen::MoQPublishError> objectStream(
      const moxygen::ObjectHeader& header,
      moxygen::Payload payload,
      bool lastInGroup = false
  ) override;

  folly::Expected<folly::Unit, moxygen::MoQPublishError>
  datagram(const moxygen::ObjectHeader& header, moxygen::Payload payload, bool lastInGroup = false)
      override;

  // Bounds how deep an interleave can be before a group is counted twice:
  // subgroups of the N most recent groups can arrive in any order.
  static constexpr size_t kRecentGroups = 3;

  // Lookup and LRU update are both linear scans on the data path, so this stays
  // an array rather than a map only while the window is tiny.
  static_assert(
      kRecentGroups <= 8,
      "widening past this wants folly::findFixed (or a map), not a longer scan"
  );

private:
  friend class TrackStatsSubgroupFilter;

  void countGroupIfNew(uint64_t groupID);
  void countSubgroup();
  void countObject(uint64_t bytes);
  void countDatagram(uint64_t bytes);
  void countBytes(uint64_t bytes);

  std::shared_ptr<stats::TrackStats> stats_;
  Direction direction_;
  // Most-recently-seen first.
  std::array<uint64_t, kRecentGroups> recentGroups_{};
  size_t recentCount_{0};
};

// Returns downstream unwrapped when no collector is bound on this thread, so
// tests and benchmarks that skip binding still publish.
std::shared_ptr<moxygen::TrackConsumer> wrapWithTrackStats(
    stats::TrackStatsRegistry& trackStats,
    const moxygen::FullTrackName& ftn,
    std::shared_ptr<moxygen::TrackConsumer> downstream,
    stats::TrackDirection direction
);

} // namespace openmoq::moqx
