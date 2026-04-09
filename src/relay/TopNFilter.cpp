/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Originally from github.com/facebookexperimental/moxygen.
 * See deps/moxygen/LICENSE for the original license terms.
 *
 * Copyright (c) OpenMOQ contributors.
 */

#include "relay/TopNFilter.h"

#include <folly/logging/xlog.h>

#include <atomic>

namespace openmoq::moqx {

namespace {
// Global tick counter for activity tracking
std::atomic<Tick> gCurrentTick{0};
} // namespace

Tick getCurrentTick() {
  return gCurrentTick.load(std::memory_order_relaxed);
}

void advanceTick() {
  gCurrentTick.fetch_add(1, std::memory_order_relaxed);
}

TopNFilter::TopNFilter(
    moxygen::FullTrackName ftn,
    std::shared_ptr<moxygen::TrackConsumer> downstream)
    : TrackConsumerFilter(downstream),
      ftn_(std::move(ftn)),
      downstream_(std::move(downstream)) {}

void TopNFilter::registerObserver(uint64_t propertyType, PropertyObserver observer) {
  XLOG(DBG4) << "[TopNFilter] Registering observer for propertyType=0x" << std::hex
             << propertyType << std::dec << " on track " << ftn_;
  observers_[propertyType] = ObserverEntry{
      .lastSeenValue = std::nullopt,
      .observer = std::move(observer),
  };
}

void TopNFilter::removeObserver(uint64_t propertyType) {
  observers_.erase(propertyType);
}

void TopNFilter::checkProperties(const moxygen::Extensions& extensions, Tick currentTick) {
  // FAST PATH: No observers and no activity tracking
  if (observers_.empty() && !activityTarget_) {
    return;
  }

  // Update activity timestamp (just a pointer write - very fast)
  if (activityTarget_) {
    *activityTarget_ = currentTick;
  }

  // FAST PATH: No observers to notify
  if (observers_.empty()) {
    return;
  }

  // Check extensions for property values
  // Optimization: iterate the smaller of observers vs extensions
  if (observers_.size() <= extensions.size()) {
    // Iterate observers, look up in extensions
    for (auto& [propertyType, entry] : observers_) {
      auto valueOpt = extensions.getIntExtension(propertyType);
      if (!valueOpt) {
        continue;
      }

      uint64_t value = *valueOpt;
      if (!entry.lastSeenValue || *entry.lastSeenValue != value) {
        XLOG(DBG4) << "[TopNFilter] Property changed on " << ftn_
                   << ": propertyType=0x" << std::hex << propertyType << std::dec
                   << " oldValue=" << entry.lastSeenValue.value_or(0)
                   << " newValue=" << value;
        entry.lastSeenValue = value;
        if (entry.observer.onValueChanged) {
          entry.observer.onValueChanged(value);
        }
      }
    }
  } else {
    // Iterate extensions, look up in observers
    // Check mutable extensions
    for (const auto& ext : extensions.getMutableExtensions()) {
      // Only check even-typed extensions (integer values)
      if (ext.type & 0x1) {
        continue;
      }
      auto it = observers_.find(ext.type);
      if (it == observers_.end()) {
        continue;
      }

      auto& entry = it->second;
      uint64_t value = ext.intValue;
      if (!entry.lastSeenValue || *entry.lastSeenValue != value) {
        entry.lastSeenValue = value;
        if (entry.observer.onValueChanged) {
          entry.observer.onValueChanged(value);
        }
      }
    }

    // Check immutable extensions
    for (const auto& ext : extensions.getImmutableExtensions()) {
      if (ext.type & 0x1) {
        continue;
      }
      auto it = observers_.find(ext.type);
      if (it == observers_.end()) {
        continue;
      }

      auto& entry = it->second;
      uint64_t value = ext.intValue;
      if (!entry.lastSeenValue || *entry.lastSeenValue != value) {
        entry.lastSeenValue = value;
        if (entry.observer.onValueChanged) {
          entry.observer.onValueChanged(value);
        }
      }
    }
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
    bool containsLastInGroup) {
  auto result =
      downstream_->beginSubgroup(groupID, subgroupID, priority, containsLastInGroup);
  if (!result) {
    return result;
  }

  // Wrap the downstream SubgroupConsumer with our filter
  return std::make_shared<TopNSubgroupConsumer>(
      shared_from_this(), std::move(result.value()));
}

folly::Expected<folly::Unit, moxygen::MoQPublishError> TopNFilter::objectStream(
    const moxygen::ObjectHeader& header,
    moxygen::Payload payload,
    bool lastInGroup) {
  // Check properties before forwarding
  checkProperties(header.extensions, getCurrentTick());
  return TrackConsumerFilter::objectStream(header, std::move(payload), lastInGroup);
}

folly::Expected<folly::Unit, moxygen::MoQPublishError> TopNFilter::datagram(
    const moxygen::ObjectHeader& header,
    moxygen::Payload payload,
    bool lastInGroup) {
  // Check properties before forwarding
  checkProperties(header.extensions, getCurrentTick());
  return TrackConsumerFilter::datagram(header, std::move(payload), lastInGroup);
}

folly::Expected<folly::Unit, moxygen::MoQPublishError> TopNFilter::publishDone(
    moxygen::PublishDone pubDone) {
  // Notify observers that the track has ended
  notifyTrackEnded();
  return TrackConsumerFilter::publishDone(std::move(pubDone));
}

// TopNSubgroupConsumer implementation

TopNSubgroupConsumer::TopNSubgroupConsumer(
    std::shared_ptr<TopNFilter> filter,
    std::shared_ptr<moxygen::SubgroupConsumer> downstream)
    : SubgroupConsumerFilter(std::move(downstream)),
      filter_(std::move(filter)) {}

folly::Expected<folly::Unit, moxygen::MoQPublishError> TopNSubgroupConsumer::object(
    uint64_t objectID,
    moxygen::Payload payload,
    moxygen::Extensions extensions,
    bool finSubgroup) {
  // Check properties before forwarding
  filter_->checkProperties(extensions, getCurrentTick());
  return SubgroupConsumerFilter::object(
      objectID, std::move(payload), std::move(extensions), finSubgroup);
}

folly::Expected<folly::Unit, moxygen::MoQPublishError> TopNSubgroupConsumer::beginObject(
    uint64_t objectID,
    uint64_t length,
    moxygen::Payload initialPayload,
    moxygen::Extensions extensions) {
  // Check properties before forwarding
  filter_->checkProperties(extensions, getCurrentTick());
  return SubgroupConsumerFilter::beginObject(
      objectID, length, std::move(initialPayload), std::move(extensions));
}

} // namespace openmoq::moqx
