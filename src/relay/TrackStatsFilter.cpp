/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/TrackStatsFilter.h"

#include <algorithm>

namespace openmoq::moqx {

using moxygen::MoQPublishError;
using moxygen::ObjectHeader;
using moxygen::Payload;
using moxygen::SubgroupConsumer;

namespace {

uint64_t payloadLength(const Payload& payload) {
  return payload ? payload->computeChainDataLength() : 0;
}

} // namespace

// Counts per-object work inside one subgroup, delegating the group/subgroup
// bookkeeping to the owning track filter.
class TrackStatsSubgroupFilter : public moxygen::SubgroupConsumerFilter {
public:
  TrackStatsSubgroupFilter(
      std::shared_ptr<TrackStatsFilter> owner,
      std::shared_ptr<SubgroupConsumer> downstream
  )
      : moxygen::SubgroupConsumerFilter(std::move(downstream)), owner_(std::move(owner)) {}

  folly::Expected<folly::Unit, MoQPublishError> object(
      uint64_t objectID,
      Payload payload,
      moxygen::Extensions extensions = moxygen::noExtensions(),
      bool finSubgroup = false
  ) override {
    owner_->countObject(payloadLength(payload));
    return moxygen::SubgroupConsumerFilter::object(
        objectID,
        std::move(payload),
        std::move(extensions),
        finSubgroup
    );
  }

  folly::Expected<folly::Unit, MoQPublishError> beginObject(
      uint64_t objectID,
      uint64_t length,
      Payload initialPayload,
      moxygen::Extensions extensions = moxygen::noExtensions()
  ) override {
    owner_->countObject(payloadLength(initialPayload));
    return moxygen::SubgroupConsumerFilter::beginObject(
        objectID,
        length,
        std::move(initialPayload),
        std::move(extensions)
    );
  }

  // endOfGroup/endOfTrackAndGroup deliver a real object (a status object with
  // no payload), so they count toward the object totals.
  folly::Expected<folly::Unit, MoQPublishError> endOfGroup(uint64_t endOfGroupObjectID) override {
    owner_->countObject(0);
    return moxygen::SubgroupConsumerFilter::endOfGroup(endOfGroupObjectID);
  }

  folly::Expected<folly::Unit, MoQPublishError> endOfTrackAndGroup(uint64_t endOfTrackObjectID
  ) override {
    owner_->countObject(0);
    return moxygen::SubgroupConsumerFilter::endOfTrackAndGroup(endOfTrackObjectID);
  }

  folly::Expected<moxygen::ObjectPublishStatus, MoQPublishError>
  objectPayload(Payload payload, bool finSubgroup = false) override {
    // Continuation of an object already counted by beginObject: bytes only.
    owner_->countBytes(payloadLength(payload));
    return moxygen::SubgroupConsumerFilter::objectPayload(std::move(payload), finSubgroup);
  }

private:
  std::shared_ptr<TrackStatsFilter> owner_;
};

TrackStatsFilter::TrackStatsFilter(
    std::shared_ptr<stats::TrackStats> stats,
    Direction direction,
    std::shared_ptr<moxygen::TrackConsumer> downstream
)
    : moxygen::TrackConsumerFilter(std::move(downstream)), stats_(std::move(stats)),
      direction_(direction) {
  if (direction_ == Direction::Egress) {
    ++stats_->counters().subscribers;
  }
}

// The consumer chain can be released from any thread, so the subscriber
// decrement and the final TrackStats ref drop both hop to the owning thread.
TrackStatsFilter::~TrackStatsFilter() {
  bool egress = direction_ == Direction::Egress;
  if (stats_->onOwnerThread()) {
    if (egress) {
      --stats_->counters().subscribers;
    }
    return;
  }
  // Must read the executor before moving stats_ out.
  auto* exec = stats_->owningExec();
  if (!exec) {
    return;
  }
  exec->add([stats = std::move(stats_), egress] {
    if (egress) {
      --stats->counters().subscribers;
    }
  });
}

folly::Expected<std::shared_ptr<SubgroupConsumer>, MoQPublishError> TrackStatsFilter::beginSubgroup(
    uint64_t groupID,
    uint64_t subgroupID,
    moxygen::Priority priority,
    moxygen::BeginSubgroupOptions options
) {
  auto res = moxygen::TrackConsumerFilter::beginSubgroup(groupID, subgroupID, priority, options);
  if (!res) {
    return res;
  }
  countGroupIfNew(groupID);
  countSubgroup();
  return std::static_pointer_cast<SubgroupConsumer>(
      std::make_shared<TrackStatsSubgroupFilter>(
          std::static_pointer_cast<TrackStatsFilter>(shared_from_this()),
          std::move(res.value())
      )
  );
}

folly::Expected<folly::Unit, MoQPublishError>
TrackStatsFilter::objectStream(const ObjectHeader& header, Payload payload, bool lastInGroup) {
  countGroupIfNew(header.group);
  countSubgroup();
  countObject(payloadLength(payload));
  return moxygen::TrackConsumerFilter::objectStream(header, std::move(payload), lastInGroup);
}

folly::Expected<folly::Unit, MoQPublishError>
TrackStatsFilter::datagram(const ObjectHeader& header, Payload payload, bool lastInGroup) {
  countGroupIfNew(header.group);
  countDatagram(payloadLength(payload));
  return moxygen::TrackConsumerFilter::datagram(header, std::move(payload), lastInGroup);
}

// LRU rather than insertion order: a group that keeps receiving subgroups stays
// in the window however many other groups open alongside it.
void TrackStatsFilter::countGroupIfNew(uint64_t groupID) {
  for (size_t i = 0; i < recentCount_; ++i) {
    if (recentGroups_[i] == groupID) {
      std::rotate(recentGroups_.begin(), recentGroups_.begin() + i, recentGroups_.begin() + i + 1);
      return;
    }
  }
  recentCount_ = std::min(recentCount_ + 1, kRecentGroups);
  std::rotate(recentGroups_.begin(), recentGroups_.end() - 1, recentGroups_.end());
  recentGroups_[0] = groupID;
  auto& counters = stats_->counters();
  if (direction_ == Direction::Ingest) {
    ++counters.groupsReceived;
  } else {
    ++counters.groupsSent;
  }
}

void TrackStatsFilter::countSubgroup() {
  auto& counters = stats_->counters();
  if (direction_ == Direction::Ingest) {
    ++counters.subgroupsReceived;
  } else {
    ++counters.subgroupsSent;
  }
}

void TrackStatsFilter::countObject(uint64_t bytes) {
  auto& counters = stats_->counters();
  if (direction_ == Direction::Ingest) {
    ++counters.objectsReceived;
  } else {
    ++counters.objectsSent;
  }
  countBytes(bytes);
}

void TrackStatsFilter::countDatagram(uint64_t bytes) {
  auto& counters = stats_->counters();
  if (direction_ == Direction::Ingest) {
    ++counters.datagramsReceived;
  } else {
    ++counters.datagramsSent;
  }
  countObject(bytes);
}

void TrackStatsFilter::countBytes(uint64_t bytes) {
  auto& counters = stats_->counters();
  if (direction_ == Direction::Ingest) {
    counters.bytesReceived += bytes;
    counters.lastObject = stats::TrackClock::now();
  } else {
    counters.bytesSent += bytes;
  }
}

std::shared_ptr<moxygen::TrackConsumer> wrapWithTrackStats(
    stats::TrackStatsRegistry& trackStats,
    const moxygen::FullTrackName& ftn,
    std::shared_ptr<moxygen::TrackConsumer> downstream,
    stats::TrackDirection direction
) {
  auto* collector = trackStats.currentCollector();
  if (!collector) {
    return downstream;
  }
  return std::static_pointer_cast<moxygen::TrackConsumer>(
      std::make_shared<TrackStatsFilter>(collector->getOrCreate(ftn), direction, std::move(downstream))
  );
}

} // namespace openmoq::moqx
