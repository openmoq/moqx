/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Originally from github.com/facebookexperimental/moxygen.
 * See deps/moxygen/LICENSE for the original license terms.
 *
 * Copyright (c) OpenMOQ contributors.
 */

#pragma once

#include <folly/container/F14Map.h>
#include <moxygen/MoQFilters.h>
#include <moxygen/MoQTypes.h>

#include <functional>
#include <memory>
#include <optional>

namespace openmoq::moqx {

// Monotonic tick counter for activity tracking.
// Using ticks is faster than std::chrono::steady_clock::now().
using Tick = uint64_t;

// Observer interface for property value changes on a track.
// Used by PropertyRanking to receive notifications when track values change.
struct PropertyObserver {
  // Called when the property value changes for this track.
  std::function<void(uint64_t value)> onValueChanged;

  // Called when the track ends (PUBLISH_DONE received).
  std::function<void()> onTrackEnded;
};

// Entry for tracking observer state per property type.
struct ObserverEntry {
  std::optional<uint64_t> lastSeenValue;
  PropertyObserver observer;
};

/**
 * TopNFilter - A TrackConsumer filter that:
 * 1. Updates activity timestamps on each object
 * 2. Observes property value changes in object extensions
 * 3. Notifies registered observers of value changes
 *
 * This is part of the TRACK_FILTER implementation for selecting top N tracks
 * based on property values (e.g., audio level).
 *
 * The filter sits in the chain: Publisher -> TopNFilter -> downstream consumer
 */
class TopNFilter : public moxygen::TrackConsumerFilter,
                   public std::enable_shared_from_this<TopNFilter> {
 public:
  explicit TopNFilter(
      moxygen::FullTrackName ftn,
      std::shared_ptr<moxygen::TrackConsumer> downstream);

  ~TopNFilter() override = default;

  // Get the full track name this filter is associated with
  const moxygen::FullTrackName& fullTrackName() const {
    return ftn_;
  }

  // Set the pointer to write activity ticks to.
  // This allows RelaySubscription to own the tick storage.
  void setActivityTarget(Tick* target) {
    activityTarget_ = target;
  }

  // Register an observer for a specific property type.
  // The observer will be called when the property value changes.
  void registerObserver(uint64_t propertyType, PropertyObserver observer);

  // Remove an observer for a property type.
  void removeObserver(uint64_t propertyType);

  // Check if any observers are registered.
  bool hasObservers() const {
    return !observers_.empty();
  }

  // Check extensions for property values and notify observers.
  // Also updates activity timestamp if activityTarget_ is set.
  // This is called by TopNSubgroupConsumer for each object.
  void checkProperties(const moxygen::Extensions& extensions, Tick currentTick);

  // Notify observers that the track has ended.
  void notifyTrackEnded();

  // Override beginSubgroup to wrap the returned SubgroupConsumer
  folly::Expected<std::shared_ptr<moxygen::SubgroupConsumer>, moxygen::MoQPublishError>
  beginSubgroup(
      uint64_t groupID,
      uint64_t subgroupID,
      moxygen::Priority priority,
      bool containsLastInGroup = false) override;

  // Override objectStream to check properties
  folly::Expected<folly::Unit, moxygen::MoQPublishError> objectStream(
      const moxygen::ObjectHeader& header,
      moxygen::Payload payload,
      bool lastInGroup = false) override;

  // Override datagram to check properties
  folly::Expected<folly::Unit, moxygen::MoQPublishError> datagram(
      const moxygen::ObjectHeader& header,
      moxygen::Payload payload,
      bool lastInGroup = false) override;

  // Override publishDone to notify observers
  folly::Expected<folly::Unit, moxygen::MoQPublishError> publishDone(
      moxygen::PublishDone pubDone) override;

 private:
  moxygen::FullTrackName ftn_;

  // Map of property type -> observer entry
  folly::F14FastMap<uint64_t, ObserverEntry> observers_;

  // Pointer to external tick storage (owned by RelaySubscription)
  Tick* activityTarget_{nullptr};

  // Reference to downstream (stored in base class, but we need it for
  // creating TopNSubgroupConsumer)
  std::shared_ptr<moxygen::TrackConsumer> downstream_;
};

/**
 * TopNSubgroupConsumer - Wraps a SubgroupConsumer to intercept objects
 * and check their extensions for property values.
 */
class TopNSubgroupConsumer : public moxygen::SubgroupConsumerFilter {
 public:
  TopNSubgroupConsumer(
      std::shared_ptr<TopNFilter> filter,
      std::shared_ptr<moxygen::SubgroupConsumer> downstream);

  // Override object to check extensions
  folly::Expected<folly::Unit, moxygen::MoQPublishError> object(
      uint64_t objectID,
      moxygen::Payload payload,
      moxygen::Extensions extensions = moxygen::noExtensions(),
      bool finSubgroup = false) override;

  // Override beginObject to check extensions
  folly::Expected<folly::Unit, moxygen::MoQPublishError> beginObject(
      uint64_t objectID,
      uint64_t length,
      moxygen::Payload initialPayload,
      moxygen::Extensions extensions = moxygen::noExtensions()) override;

 private:
  std::shared_ptr<TopNFilter> filter_;
};

// Get current tick value. This is a simple monotonic counter.
// In production, this would typically be derived from a high-resolution
// timer or rdtsc. For now, we use a simple atomic counter that can be
// advanced by the caller (e.g., HHWheelTimer callback).
Tick getCurrentTick();

// Advance the global tick counter. Called by timer callbacks.
void advanceTick();

} // namespace openmoq::moqx
