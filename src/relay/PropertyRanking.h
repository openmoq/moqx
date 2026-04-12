/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <moxygen/MoQSession.h>
#include <moxygen/MoQTypes.h>

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace openmoq::moqx {

/**
 * Ranking key for deterministic ordering in std::map.
 * Uses std::greater<> comparator for descending order (highest value first).
 */
struct RankKey {
  uint64_t value;      // Property value (higher = better rank)
  uint64_t arrivalSeq; // Tie-breaker (lower = earlier = wins)

  bool operator<(const RankKey& other) const {
    if (value != other.value) {
      return value < other.value;
    }
    return arrivalSeq > other.arrivalSeq;
  }

  bool operator>(const RankKey& other) const {
    if (value != other.value) {
      return value > other.value;
    }
    return arrivalSeq < other.arrivalSeq;
  }

  bool operator==(const RankKey& other) const {
    return value == other.value && arrivalSeq == other.arrivalSeq;
  }

  bool operator!=(const RankKey& other) const { return !(*this == other); }

  bool operator<=(const RankKey& other) const { return *this < other || *this == other; }

  bool operator>=(const RankKey& other) const { return *this > other || *this == other; }
};

/**
 * Entry stored in the sorted container.
 */
struct RankedEntry {
  moxygen::FullTrackName ftn;
  std::weak_ptr<moxygen::MoQSession> publisher;
};

/**
 * Fast lookup from track name to rank position.
 * Includes cached rank to avoid O(n) std::distance() calls.
 */
struct RankIndex {
  std::map<RankKey, RankedEntry, std::greater<RankKey>>::iterator rankIter;
  uint64_t cachedRank{UINT64_MAX};
};

/**
 * Track state within a TopNGroup.
 */
enum class TrackState { Selected, Deselected };

/**
 * Per-session state within a TopNGroup.
 */
struct SessionInfo {
  bool forward{true};

  // Waterline for self-exclusion (publisher-subscribers only).
  // Always current when isPublisher(); recomputed by reconcilePublisherSelection.
  // nullopt = fewer than N non-self tracks exist — select all non-self tracks.
  std::optional<RankKey> waterlineKey;

  // Tracks published by this session in the subscribed namespace.
  // Empty = viewer (no self-exclusion needed).
  folly::F14FastSet<moxygen::FullTrackName, moxygen::FullTrackName::hash> publishedTracks;

  // Tracks currently being delivered to this publisher-subscriber.
  // Used by reconcilePublisherSelection to compute the select/evict delta.
  // Seeded from the shared top-N when a viewer transitions to publisher.
  folly::F14FastSet<moxygen::FullTrackName, moxygen::FullTrackName::hash> selectedTracks;

  bool isPublisher() const { return !publishedTracks.empty(); }

  bool isSelfTrack(const moxygen::FullTrackName& ftn) const {
    return publishedTracks.find(ftn) != publishedTracks.end();
  }
};

/**
 * TopNGroup - Group of subscribers with the same N value.
 */
struct TopNGroup {
  uint64_t maxSelected{0}; // N

  // Session -> state mapping
  folly::F14FastMap<std::shared_ptr<moxygen::MoQSession>, SessionInfo> sessions;

  // Track -> state (Selected or Deselected)
  folly::F14FastMap<moxygen::FullTrackName, TrackState, moxygen::FullTrackName::hash> trackStates;

  // FIFO queue for cheap reselection.
  // Bounded by maxDeselected_ (a relay-level property) before eviction.
  std::deque<moxygen::FullTrackName> deselectedQueue;
};

/**
 * PropertyRanking - Manages ranking of tracks by a property value.
 *
 * Maintains a sorted map of tracks by their property values and notifies
 * subscribers when tracks enter or leave their top-N selection.
 *
 * Key design decisions:
 * - std::map with std::greater<> for O(log T) sorted iteration (descending)
 * - F14FastMap for O(1) track lookups
 * - Fast path when value change doesn't cross any threshold
 * - Deselected queue for cheap reselection without PUBLISH_DONE
 */
class PropertyRanking {
public:
  using SelectCallback = std::function<void(
      const moxygen::FullTrackName& ftn,
      std::shared_ptr<moxygen::MoQSession> session,
      bool forward
  )>;

  using EvictCallback = std::function<
      void(const moxygen::FullTrackName& ftn, std::shared_ptr<moxygen::MoQSession> session)>;

  // Batch notification callback for viewer sessions.
  // Called once per track state change with all affected sessions.
  using BatchSelectCallback = std::function<void(
      const moxygen::FullTrackName& ftn,
      const std::vector<std::pair<std::shared_ptr<moxygen::MoQSession>, bool>>& sessions
  )>;

  // Returns the last activity time for a track. Called during sweepIdle().
  // If the track is unknown, return a default-constructed time_point (treated
  // as always-idle).
  using GetLastActivityFn =
      std::function<std::chrono::steady_clock::time_point(const moxygen::FullTrackName&)>;

  /**
   * @param propertyType    The property type ID to rank by
   * @param maxDeselected   Maximum tracks in deselected queue before eviction
   * @param idleTimeout     How long a selected track may be silent before
   *                        being deselected. Zero disables idle eviction.
   * @param getLastActivity Relay callback to read per-track activity time
   * @param onBatchSelected Batch callback for viewer notifications (required)
   * @param onSelected      Individual callback for per-session notifications
   * @param onEvicted       Callback when track is evicted from deselected queue
   */
  PropertyRanking(
      uint64_t propertyType,
      uint64_t maxDeselected,
      std::chrono::milliseconds idleTimeout,
      GetLastActivityFn getLastActivity,
      BatchSelectCallback onBatchSelected,
      SelectCallback onSelected,
      EvictCallback onEvicted
  );

  ~PropertyRanking() = default;

  PropertyRanking(const PropertyRanking&) = delete;
  PropertyRanking& operator=(const PropertyRanking&) = delete;
  PropertyRanking(PropertyRanking&&) = delete;
  PropertyRanking& operator=(PropertyRanking&&) = delete;

  uint64_t propertyType() const { return propertyType_; }

  /**
   * Register a track. Called at publish() time.
   */
  void registerTrack(
      const moxygen::FullTrackName& ftn,
      std::optional<uint64_t> initialValue,
      std::weak_ptr<moxygen::MoQSession> publisher
  );

  /**
   * Update track's sort value. Main hot-path entry point.
   * Returns quickly (fast path) if value doesn't cross threshold.
   */
  void updateSortValue(const moxygen::FullTrackName& ftn, uint64_t value);

  /**
   * Remove track (on PUBLISH_DONE).
   */
  void removeTrack(const moxygen::FullTrackName& ftn);

  /**
   * Get or create TopNGroup for given N.
   */
  TopNGroup& getOrCreateTopNGroup(uint64_t maxSelected);

  /**
   * Add a session to a TopNGroup.
   * If publishedTracks is non-empty, enables self-exclusion for this session:
   * the session will not receive notifications for its own published tracks.
   */
  void addSessionToTopNGroup(
      uint64_t maxSelected,
      std::shared_ptr<moxygen::MoQSession> session,
      bool forward,
      std::vector<moxygen::FullTrackName> publishedTracks = {}
  );

  /**
   * Remove a session from a TopNGroup.
   */
  void removeSessionFromTopNGroup(
      uint64_t maxSelected,
      const std::shared_ptr<moxygen::MoQSession>& session
  );

  /**
   * Add a published track to a session's self-exclusion list.
   * Call this when a publisher-subscriber publishes a new track after
   * already subscribing with TRACK_FILTER.
   */
  void addPublishedTrackToSession(
      uint64_t maxSelected,
      const std::shared_ptr<moxygen::MoQSession>& session,
      const moxygen::FullTrackName& ftn
  );

  /**
   * Remove a published track from a session's self-exclusion list.
   * Call this when a publisher-subscriber's track ends (PUBLISH_DONE).
   */
  void removePublishedTrackFromSession(
      uint64_t maxSelected,
      const std::shared_ptr<moxygen::MoQSession>& session,
      const moxygen::FullTrackName& ftn
  );

  /**
   * Remove TopNGroup (last subscriber with this N left).
   */
  void removeTopNGroup(uint64_t maxSelected);

  bool empty() const { return topNGroups_.empty(); }

  /**
   * Sweep all selected tracks for idleness. Any track whose last activity
   * time is older than idleTimeout_ is deselected into the deselectedQueue
   * (same path as ranking-based demotion — cheap reselection still works).
   * No-op when idleTimeout_ is zero.
   * Called from onActivity observer (already throttled by TopNFilter) and
   * opportunistically from updateSortValue().
   */
  void sweepIdle();

  size_t numTracks() const { return trackIndexByName_.size(); }
  size_t numTopNGroups() const { return topNGroups_.size(); }

  // Test accessors
  const std::map<RankKey, RankedEntry, std::greater<RankKey>>& rankedTracks() const {
    return rankedTracks_;
  }

  const TopNGroup* getTopNGroup(uint64_t maxSelected) const {
    auto it = topNGroups_.find(maxSelected);
    return it != topNGroups_.end() ? &it->second : nullptr;
  }

private:
  uint64_t getRank(const RankKey& key) const;

  // Check whether a rank move from oldRank to newRank crosses any selection
  // threshold. Both ranks must be computed against the same (post-update) map.
  bool crossesThreshold(uint64_t oldRank, uint64_t newRank) const;

  // Recompute TopNGroup state after a rank move. oldRank/newRank are the
  // ranks captured before and after the map update respectively.
  void recomputeTopNGroups(const moxygen::FullTrackName& ftn, uint64_t oldRank, uint64_t newRank);

  void updateSelectionThreshold();

  void trimDeselectedQueue(TopNGroup& topNGroup);

  /**
   * Notify all sessions in a TopNGroup about a newly selected track.
   * Must be called with iteratingSessions_ guard active.
   */
  void notifyTrackSelected(const moxygen::FullTrackName& ftn, TopNGroup& topNGroup);

  void rebuildRankCacheIfNeeded() const;
  uint64_t getCachedRank(const moxygen::FullTrackName& ftn) const;

  // Remove ftn from the deselected queue (if present). O(queue size).
  void removeFromDeselectedQueue(TopNGroup& group, const moxygen::FullTrackName& ftn);

  // If the track at rank n in rankedTracks_ is marked Selected, move it to the
  // deselected queue and trim. Call this after a track enters top-N and pushes
  // the previous occupant of rank N-1 down to rank N.
  void demoteTrackAtRank(uint64_t n, TopNGroup& group);

  // Compute the waterline key for a publisher-subscriber session.
  // Returns the RankKey of the Nth non-self track (the lowest-ranked track
  // that should still be selected for this session). Returns nullopt if
  // fewer than N non-self tracks exist (meaning all non-self tracks are selected).
  std::optional<RankKey> computeWaterlineKey(const SessionInfo& info, uint64_t maxSelected) const;

  // Recompute the publisher's personal top-N and fire callbacks for the delta.
  // Evicts tracks that left the selection; selects tracks that entered it.
  // Updates info.waterlineKey and info.selectedTracks to reflect the new state.
  void reconcilePublisherSelection(
      SessionInfo& info,
      uint64_t maxSelected,
      const std::shared_ptr<moxygen::MoQSession>& session
  );

  void invalidateRankCache() { rankCacheValid_ = false; }

  uint64_t propertyType_;
  uint64_t maxDeselected_;
  std::chrono::milliseconds idleTimeout_;
  GetLastActivityFn getLastActivity_;
  BatchSelectCallback onBatchSelected_;
  SelectCallback onSelected_;
  EvictCallback onEvicted_;

  std::map<RankKey, RankedEntry, std::greater<RankKey>> rankedTracks_;
  // Name → iterator/rank index into rankedTracks_. O(1) lookup by track name.
  folly::F14FastMap<moxygen::FullTrackName, RankIndex, moxygen::FullTrackName::hash>
      trackIndexByName_;
  folly::F14FastMap<uint64_t, TopNGroup> topNGroups_;

  uint64_t nextSeq_{0};
  uint64_t selectionThreshold_{0};
  std::vector<uint64_t> sortedThresholds_;

  mutable bool rankCacheValid_{false};

  bool iteratingSessions_{false};

  class IterationGuard {
  public:
    explicit IterationGuard(PropertyRanking& pr) : pr_(pr) { pr_.iteratingSessions_ = true; }
    ~IterationGuard() { pr_.iteratingSessions_ = false; }
    IterationGuard(const IterationGuard&) = delete;
    IterationGuard& operator=(const IterationGuard&) = delete;

  private:
    PropertyRanking& pr_;
  };
};

} // namespace openmoq::moqx
