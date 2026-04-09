/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Originally from github.com/facebookexperimental/moxygen.
 * See deps/moxygen/LICENSE for the original license terms.
 *
 * Copyright (c) OpenMOQ contributors.
 */

#pragma once

#include <folly/container/F14Map.h>
#include <folly/io/async/HHWheelTimer.h>
#include <moxygen/MoQSession.h>
#include <moxygen/MoQTypes.h>
#include "TopNFilter.h"

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>

namespace openmoq::moqx {

/**
 * Ranking key for deterministic ordering in std::map.
 * Stored with negated value for descending order (highest value first).
 */
struct RankKey {
  int64_t negValue;    // Negated for descending order
  uint64_t arrivalSeq; // Tie-breaker (lower = earlier = wins)

  bool operator<(const RankKey& other) const {
    if (negValue != other.negValue) {
      return negValue < other.negValue;
    }
    return arrivalSeq < other.arrivalSeq;
  }

  bool operator==(const RankKey& other) const {
    return negValue == other.negValue && arrivalSeq == other.arrivalSeq;
  }

  bool operator!=(const RankKey& other) const {
    return !(*this == other);
  }
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
struct TrackEntry {
  std::map<RankKey, RankedEntry>::iterator rankIter;
  uint64_t cachedRank{UINT64_MAX};  // Cached rank position, invalidated on changes
};

/**
 * Track state within a group.
 */
enum class TrackState { Selected, Deselected };

/**
 * Per-session state within a TopNGroup.
 */
struct SessionInfo {
  bool forward{true};

  // Waterline for self-exclusion (publisher-subscribers only).
  // nullopt = use shared threshold (viewers, or PS with no self-tracks)
  std::optional<RankKey> waterlineKey;

  // Optimization: track whether waterline needs recomputation.
  // Only invalidate when self-tracks change position or are added/removed.
  bool waterlineValid{false};

  // Tracks published by this session (for self-exclusion).
  // Empty = viewer (no self-exclusion needed)
  folly::F14FastSet<moxygen::FullTrackName, moxygen::FullTrackName::hash> publishedTracks;

  bool isPublisher() const {
    return !publishedTracks.empty();
  }

  bool isSelfTrack(const moxygen::FullTrackName& ftn) const {
    return publishedTracks.find(ftn) != publishedTracks.end();
  }
};

/**
 * Group of subscribers with same N value.
 */
struct TopNGroup {
  uint64_t maxSelected{0}; // N

  // Session -> state mapping
  folly::F14FastMap<std::shared_ptr<moxygen::MoQSession>, SessionInfo> sessions;

  // Track -> state (Selected or Deselected)
  folly::F14FastMap<moxygen::FullTrackName, TrackState, moxygen::FullTrackName::hash> trackStates;

  // FIFO queue for cheap reselection (bounded by maxDeselected_)
  std::deque<moxygen::FullTrackName> deselectedQueue;
};

/**
 * Performance metrics for PropertyRanking operations.
 * Only collected when metricsEnabled() is true (disabled by default).
 */
struct PropertyRankingMetrics {
  // Track operations
  std::atomic<uint64_t> tracksRegistered{0};
  std::atomic<uint64_t> tracksRemoved{0};
  std::atomic<uint64_t> valueUpdates{0};
  std::atomic<uint64_t> fastPathHits{0};    // Updates that didn't cross threshold
  std::atomic<uint64_t> slowPathHits{0};    // Updates that required recomputation

  // Selection operations
  std::atomic<uint64_t> selectionsTriggered{0};
  std::atomic<uint64_t> evictionsTriggered{0};
  std::atomic<uint64_t> selfExclusionSkips{0};  // Tracks skipped due to self-exclusion

  // Session operations
  std::atomic<uint64_t> sessionsAdded{0};
  std::atomic<uint64_t> sessionsRemoved{0};

  // Timing (nanoseconds, accumulated) - only when timing enabled
  std::atomic<uint64_t> updateSortValueTimeNs{0};
  std::atomic<uint64_t> recomputeGroupsTimeNs{0};
  std::atomic<uint64_t> notifyTimeNs{0};

  // Idle sweep
  std::atomic<uint64_t> idleSweeps{0};
  std::atomic<uint64_t> idleDemotions{0};

  // Control flags
  bool enabled{false};      // Set to true to collect counters
  bool timingEnabled{false}; // Set to true to collect timing (more overhead)
};

/**
 * PropertyRanking - Manages ranking of tracks by a property value.
 *
 * Maintains a sorted map of tracks by their property values and notifies
 * subscribers when tracks enter or leave their top-N selection.
 *
 * Key design decisions:
 * - std::map for O(log T) sorted iteration
 * - F14FastMap for O(1) track lookups
 * - Fast path when value change doesn't cross any threshold
 * - Deselected queue for cheap reselection without PUBLISH_DONE
 * - HHWheelTimer for idle sweep (demotes inactive selected tracks)
 */
class PropertyRanking : public folly::HHWheelTimer::Callback {
 public:
  using SelectCallback = std::function<void(
      const moxygen::FullTrackName& ftn,
      std::shared_ptr<moxygen::MoQSession> session,
      bool forward)>;

  using EvictCallback = std::function<void(
      const moxygen::FullTrackName& ftn,
      std::shared_ptr<moxygen::MoQSession> session)>;

  // Batch notification callback for viewers (optimization).
  // Called once per track state change with all affected viewer sessions.
  using BatchSelectCallback = std::function<void(
      const moxygen::FullTrackName& ftn,
      const std::vector<std::pair<std::shared_ptr<moxygen::MoQSession>, bool>>& sessions)>;

  using GetLastActivityFn = std::function<Tick(const moxygen::FullTrackName& ftn)>;

  /**
   * Constructor without idle detection (for testing or non-timer use cases).
   */
  PropertyRanking(
      uint64_t propertyType,
      uint64_t maxDeselected,
      SelectCallback onSelected,
      EvictCallback onEvicted);

  /**
   * Constructor with idle detection.
   * @param idleThreshold Number of ticks before a track is considered idle
   * @param sweepInterval How often to run the idle sweep
   * @param timer The HHWheelTimer to schedule sweep callbacks on
   * @param getLastActivity Callback to get last activity tick for a track
   */
  PropertyRanking(
      uint64_t propertyType,
      uint64_t maxDeselected,
      SelectCallback onSelected,
      EvictCallback onEvicted,
      Tick idleThreshold,
      std::chrono::milliseconds sweepInterval,
      folly::HHWheelTimer& timer,
      GetLastActivityFn getLastActivity);

  ~PropertyRanking() override;

  // Non-copyable, non-movable
  PropertyRanking(const PropertyRanking&) = delete;
  PropertyRanking& operator=(const PropertyRanking&) = delete;
  PropertyRanking(PropertyRanking&&) = delete;
  PropertyRanking& operator=(PropertyRanking&&) = delete;

  uint64_t propertyType() const {
    return propertyType_;
  }

  /**
   * Set optional batch callback for viewer notifications.
   * When set, viewer notifications are batched into a single call per track.
   */
  void setBatchSelectCallback(BatchSelectCallback callback) {
    onBatchSelected_ = std::move(callback);
  }

  /**
   * Register a track with optional initial value.
   * Called at publish() time.
   */
  void registerTrack(
      const moxygen::FullTrackName& ftn,
      std::optional<uint64_t> initialValue,
      std::weak_ptr<moxygen::MoQSession> publisher = {});

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
   * Get or create group for given N.
   */
  TopNGroup& getOrCreateGroup(uint64_t maxSelected);

  /**
   * Add a session to a group.
   * If publishedTracks is provided, enables self-exclusion for this session.
   */
  void addSessionToGroup(
      uint64_t maxSelected,
      std::shared_ptr<moxygen::MoQSession> session,
      bool forward,
      std::vector<moxygen::FullTrackName> publishedTracks = {});

  /**
   * Remove a session from a group.
   */
  void removeSessionFromGroup(
      uint64_t maxSelected,
      const std::shared_ptr<moxygen::MoQSession>& session);

  /**
   * Add a published track to a session's self-exclusion list.
   * Call this when a publisher-subscriber publishes a new track.
   */
  void addPublishedTrackToSession(
      uint64_t maxSelected,
      const std::shared_ptr<moxygen::MoQSession>& session,
      const moxygen::FullTrackName& ftn);

  /**
   * Remove a published track from a session's self-exclusion list.
   * Call this when a publisher-subscriber's track ends.
   */
  void removePublishedTrackFromSession(
      uint64_t maxSelected,
      const std::shared_ptr<moxygen::MoQSession>& session,
      const moxygen::FullTrackName& ftn);

  /**
   * Remove group (last subscriber with this N left).
   */
  void removeGroup(uint64_t maxSelected);

  /**
   * Check if any groups remain.
   */
  bool empty() const {
    return groups_.empty();
  }

  /**
   * Get number of registered tracks.
   */
  size_t numTracks() const {
    return tracks_.size();
  }

  /**
   * Get number of groups.
   */
  size_t numGroups() const {
    return groups_.size();
  }

  // Test accessors
  const std::map<RankKey, RankedEntry>& ranked() const {
    return ranked_;
  }

  const TopNGroup* getGroup(uint64_t maxSelected) const {
    auto it = groups_.find(maxSelected);
    return it != groups_.end() ? &it->second : nullptr;
  }

  /**
   * Get performance metrics snapshot.
   * Metrics are only collected when enabled via enableMetrics().
   */
  const PropertyRankingMetrics& metrics() const {
    return metrics_;
  }

  /**
   * Enable/disable metrics collection (disabled by default for performance).
   * @param timing Also enable timing measurements (higher overhead)
   */
  void enableMetrics(bool timing = false) {
    metrics_.enabled = true;
    metrics_.timingEnabled = timing;
  }

  void disableMetrics() {
    metrics_.enabled = false;
    metrics_.timingEnabled = false;
  }

  bool metricsEnabled() const {
    return metrics_.enabled;
  }

  /**
   * Start the idle sweep timer. Called when first group is created.
   */
  void startIdleSweep();

  /**
   * Stop the idle sweep timer. Called when last group is removed.
   */
  void stopIdleSweep();

  /**
   * HHWheelTimer callback - runs idle sweep.
   */
  void timeoutExpired() noexcept override;

  /**
   * Cancel the timer callback (called from destructor).
   */
  void callbackCanceled() noexcept override {}

 private:
  /**
   * Get the 0-based rank position of a key.
   */
  uint64_t getRank(const RankKey& key) const;

  /**
   * Check if value crosses any threshold (fast-path gate).
   */
  bool crossesThreshold(
      std::optional<RankKey> oldKey,
      const RankKey& newKey) const;

  /**
   * Recompute all groups after a rank change.
   */
  void recomputeGroups(
      const moxygen::FullTrackName& ftn,
      std::optional<RankKey> oldKey,
      const RankKey& newKey);

  /**
   * Update pool boundary when groups change.
   * Pool boundary = max(N) + maxDeselected_
   */
  void updatePoolBoundary();

  /**
   * Evict oldest track from deselected queue if over limit.
   */
  void trimDeselectedQueue(TopNGroup& group);

  /**
   * Compute waterline key for self-exclusion.
   * Returns the RankKey of the Nth non-self track for this session.
   */
  std::optional<RankKey> computeWaterlineKey(
      const SessionInfo& info,
      uint64_t maxSelected) const;

  /**
   * Check if a track should be selected for a session (considers self-exclusion).
   */
  bool shouldSelectForSession(
      const moxygen::FullTrackName& ftn,
      const RankKey& key,
      const SessionInfo& info,
      uint64_t maxSelected) const;

  /**
   * Sweep for idle selected tracks and demote them.
   */
  void sweepIdle();

  /**
   * Rebuild rank cache after structural changes (insert/delete).
   * O(T) but amortized by lazy rebuilding.
   */
  void rebuildRankCacheIfNeeded() const;

  /**
   * Get rank from cache, rebuilding if invalidated.
   * O(1) amortized.
   */
  uint64_t getCachedRank(const moxygen::FullTrackName& ftn) const;

  /**
   * Invalidate the rank cache. Called on insert/delete.
   */
  void invalidateRankCache() {
    rankCacheValid_ = false;
  }

  uint64_t propertyType_;
  uint64_t maxDeselected_;
  SelectCallback onSelected_;
  EvictCallback onEvicted_;
  BatchSelectCallback onBatchSelected_;  // Optional batch callback for viewers

  // Sorted container: (negValue, seq) -> RankedEntry
  std::map<RankKey, RankedEntry> ranked_;

  // O(1) lookup: trackName -> iterator into ranked_
  folly::F14FastMap<moxygen::FullTrackName, TrackEntry, moxygen::FullTrackName::hash> tracks_;

  // Groups by maxSelected value (N)
  folly::F14FastMap<uint64_t, TopNGroup> groups_;

  // Monotonic sequence for tie-breaking
  uint64_t nextSeq_{0};

  // Cached pool boundary (max N + maxDeselected_)
  uint64_t poolBoundary_{0};

  // Sorted threshold values for O(log G) crossesThreshold check
  std::vector<uint64_t> sortedThresholds_;

  // Idle detection fields (optional - only set if timer constructor used)
  Tick idleThreshold_{0};
  std::chrono::milliseconds sweepInterval_{0};
  folly::HHWheelTimer* timer_{nullptr};
  GetLastActivityFn getLastActivity_;
  bool sweepRunning_{false};

  // Performance metrics
  mutable PropertyRankingMetrics metrics_;

  // Rank cache invalidation flag - mutable because cache rebuild is const-safe
  mutable bool rankCacheValid_{false};
};

} // namespace openmoq::moqx
