/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/container/F14Map.h>
#include <moxygen/MoQFilters.h>
#include <moxygen/MoQTypes.h>

#include <functional>
#include <memory>
#include <optional>

namespace openmoq::moqx {

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
 * 1. Observes property value changes in object extensions
 * 2. Notifies registered observers of value changes
 * 3. Handles PUBLISH_DONE to notify observers that the track ended
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
      std::shared_ptr<moxygen::TrackConsumer> downstream
  );

  ~TopNFilter() override = default;

  // Get the full track name this filter is associated with
  const moxygen::FullTrackName& fullTrackName() const { return ftn_; }

  // Register an observer for a specific property type.
  // Only one observer per property type is supported; registering a second
  // observer for the same type silently overwrites the first.
  void registerObserver(uint64_t propertyType, PropertyObserver observer);

  // Remove an observer for a property type.
  void removeObserver(uint64_t propertyType);

  // Check if any observers are registered.
  bool hasObservers() const { return !observers_.empty(); }

  // Check extensions for property values and notify observers.
  // This is called for each object received.
  void checkProperties(const moxygen::Extensions& extensions);

  // Notify observers that the track has ended.
  void notifyTrackEnded();

  // Check if publishDone has been received
  bool isEnded() const { return ended_; }

  // Override beginSubgroup to wrap the returned SubgroupConsumer
  folly::Expected<std::shared_ptr<moxygen::SubgroupConsumer>, moxygen::MoQPublishError>
  beginSubgroup(
      uint64_t groupID,
      uint64_t subgroupID,
      moxygen::Priority priority,
      bool containsLastInGroup = false
  ) override;

  // Override objectStream to check properties
  folly::Expected<folly::Unit, moxygen::MoQPublishError> objectStream(
      const moxygen::ObjectHeader& header,
      moxygen::Payload payload,
      bool lastInGroup = false
  ) override;

  // Override datagram to check properties
  folly::Expected<folly::Unit, moxygen::MoQPublishError>
  datagram(const moxygen::ObjectHeader& header, moxygen::Payload payload, bool lastInGroup = false)
      override;

  // Override publishDone to notify observers
  folly::Expected<folly::Unit, moxygen::MoQPublishError> publishDone(moxygen::PublishDone pubDone
  ) override;

private:
  moxygen::FullTrackName ftn_;

  // Map of property type -> observer entry
  folly::F14FastMap<uint64_t, ObserverEntry> observers_;

  // Reference to downstream (stored in base class, but we need it for
  // creating TopNSubgroupConsumer)
  std::shared_ptr<moxygen::TrackConsumer> downstream_;

  // Track whether publishDone has been received
  bool ended_{false};
};

/**
 * TopNSubgroupConsumer - Wraps a SubgroupConsumer to intercept objects
 * and check their extensions for property values.
 */
class TopNSubgroupConsumer : public moxygen::SubgroupConsumerFilter {
public:
  TopNSubgroupConsumer(
      std::shared_ptr<TopNFilter> filter,
      std::shared_ptr<moxygen::SubgroupConsumer> downstream
  );

  // Override object to check extensions
  folly::Expected<folly::Unit, moxygen::MoQPublishError> object(
      uint64_t objectID,
      moxygen::Payload payload,
      moxygen::Extensions extensions = moxygen::noExtensions(),
      bool finSubgroup = false
  ) override;

  // Override beginObject to check extensions
  folly::Expected<folly::Unit, moxygen::MoQPublishError> beginObject(
      uint64_t objectID,
      uint64_t length,
      moxygen::Payload initialPayload,
      moxygen::Extensions extensions = moxygen::noExtensions()
  ) override;

private:
  std::shared_ptr<TopNFilter> filter_;
};

} // namespace openmoq::moqx
