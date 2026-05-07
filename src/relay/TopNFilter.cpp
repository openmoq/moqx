/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/TopNFilter.h"

#include <folly/logging/xlog.h>

namespace openmoq::moqx {

TopNFilter::TopNFilter(
    moxygen::FullTrackName ftn,
    std::shared_ptr<moxygen::TrackConsumer> downstream
)
    : TrackConsumerFilter(downstream), ftn_(std::move(ftn)), downstream_(std::move(downstream)) {}

void TopNFilter::registerObserver(uint64_t propertyType, PropertyObserver observer) {
  XLOG(DBG4) << "[TopNFilter] Registering observer for propertyType=0x" << std::hex << propertyType
             << std::dec << " on track " << ftn_;
  observers_[propertyType] = ObserverEntry{
      .lastSeenValue = std::nullopt,
      .observer = std::move(observer),
  };
}

void TopNFilter::removeObserver(uint64_t propertyType) {
  observers_.erase(propertyType);
}

void TopNFilter::setActivityTarget(std::chrono::steady_clock::time_point* target) {
  activityTarget_ = target;
}

void TopNFilter::setActivityThreshold(std::chrono::milliseconds threshold) {
  activityThreshold_ = threshold;
}

void TopNFilter::checkProperties(const moxygen::Extensions& extensions) {
  // FAST PATH: No observers — nothing to do
  if (observers_.empty()) {
    return;
  }

  auto now = std::chrono::steady_clock::now();

  // Throttle check for onActivity
  bool activityThrottleAllows =
      activityThreshold_.count() > 0 &&
      (!lastActivityNotify_ || now - *lastActivityNotify_ >= activityThreshold_);
  bool firedAnyActivity = false;

  // Check extensions for property values we're observing
  for (auto& [propertyType, entry] : observers_) {
    auto valueOpt = extensions.getIntExtension(propertyType);
    if (!valueOpt) {
      continue;
    }

    // Property matched — update activity timestamp
    if (activityTarget_) {
      *activityTarget_ = now;
    }

    uint64_t value = *valueOpt;
    if (!entry.lastSeenValue || *entry.lastSeenValue != value) {
      // Value changed: fire onValueChanged
      XLOG(DBG4) << "[TopNFilter] Property changed on " << ftn_ << ": propertyType=0x" << std::hex
                 << propertyType << std::dec << " oldValue=" << entry.lastSeenValue.value_or(0)
                 << " newValue=" << value;
      entry.lastSeenValue = value;
      if (entry.observer.onValueChanged) {
        entry.observer.onValueChanged(value);
      }
    }
    // Fire onActivity regardless of whether value changed (throttled).
    // Two-stop throttle: TopNFilter throttles per-track; PropertyRanking::sweepIdle
    // throttles globally so repeated onActivity calls from many tracks are cheap.
    if (activityThrottleAllows && entry.observer.onActivity) {
      entry.observer.onActivity();
      firedAnyActivity = true;
    }
  }

  if (firedAnyActivity) {
    lastActivityNotify_ = now;
  }
}

void TopNFilter::notifyTrackEnded() {
  XLOG(DBG4) << "[TopNFilter] Track ended: " << ftn_;
  for (auto& [propertyType, entry] : observers_) {
    if (entry.observer.onTrackEnded) {
      entry.observer.onTrackEnded();
    }
  }
}

folly::Expected<std::shared_ptr<moxygen::SubgroupConsumer>, moxygen::MoQPublishError>
TopNFilter::beginSubgroup(
    uint64_t groupID,
    uint64_t subgroupID,
    moxygen::Priority priority,
    bool containsLastInGroup
) {
  // Handle objects arriving after publishDone - this can happen due to
  // out-of-order delivery. We still forward them but log a warning.
  if (ended_) {
    XLOG(WARN) << "[TopNFilter] beginSubgroup received after publishDone on " << ftn_;
  }

  auto result = downstream_->beginSubgroup(groupID, subgroupID, priority, containsLastInGroup);
  if (!result) {
    return result;
  }

  // Wrap the downstream SubgroupConsumer with our filter
  return std::make_shared<TopNSubgroupConsumer>(shared_from_this(), std::move(result.value()));
}

folly::Expected<folly::Unit, moxygen::MoQPublishError> TopNFilter::objectStream(
    const moxygen::ObjectHeader& header,
    moxygen::Payload payload,
    bool lastInGroup
) {
  if (ended_) {
    XLOG(WARN) << "[TopNFilter] objectStream received after publishDone on " << ftn_;
  }
  // Check properties before forwarding
  checkProperties(header.extensions);
  return TrackConsumerFilter::objectStream(header, std::move(payload), lastInGroup);
}

folly::Expected<folly::Unit, moxygen::MoQPublishError> TopNFilter::datagram(
    const moxygen::ObjectHeader& header,
    moxygen::Payload payload,
    bool lastInGroup
) {
  if (ended_) {
    XLOG(WARN) << "[TopNFilter] datagram received after publishDone on " << ftn_;
  }
  // Check properties before forwarding
  checkProperties(header.extensions);
  return TrackConsumerFilter::datagram(header, std::move(payload), lastInGroup);
}

folly::Expected<folly::Unit, moxygen::MoQPublishError>
TopNFilter::publishDone(moxygen::PublishDone pubDone) {
  // Mark as ended before notifying observers
  ended_ = true;
  // Notify observers that the track has ended
  notifyTrackEnded();
  return TrackConsumerFilter::publishDone(std::move(pubDone));
}

// TopNSubgroupConsumer implementation

TopNSubgroupConsumer::TopNSubgroupConsumer(
    std::shared_ptr<TopNFilter> filter,
    std::shared_ptr<moxygen::SubgroupConsumer> downstream
)
    : SubgroupConsumerFilter(std::move(downstream)), filter_(std::move(filter)) {}

folly::Expected<folly::Unit, moxygen::MoQPublishError> TopNSubgroupConsumer::object(
    uint64_t objectID,
    moxygen::Payload payload,
    moxygen::Extensions extensions,
    bool finSubgroup
) {
  // Check properties before forwarding
  filter_->checkProperties(extensions);
  return SubgroupConsumerFilter::object(
      objectID,
      std::move(payload),
      std::move(extensions),
      finSubgroup
  );
}

folly::Expected<folly::Unit, moxygen::MoQPublishError> TopNSubgroupConsumer::beginObject(
    uint64_t objectID,
    uint64_t length,
    moxygen::Payload initialPayload,
    moxygen::Extensions extensions
) {
  // Check properties before forwarding
  filter_->checkProperties(extensions);
  return SubgroupConsumerFilter::beginObject(
      objectID,
      length,
      std::move(initialPayload),
      std::move(extensions)
  );
}

} // namespace openmoq::moqx
