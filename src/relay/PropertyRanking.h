/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Originally from github.com/facebookexperimental/moxygen.
 * See deps/moxygen/LICENSE for the original license terms.
 *
 * Copyright (c) OpenMOQ contributors.
 */

#pragma once

#include <folly/container/F14Map.h>
#include <moxygen/MoQSession.h>
#include <moxygen/MoQTypes.h>

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>

namespace openmoq::moqx {

/**
 * Ranking key for deterministic ordering in std::map.
 * Uses std::greater<> comparator for descending order (highest value first).
 */
struct RankKey {
  uint64_t value;      // Property value (higher = better rank)
  uint64_t arrivalSeq; // Tie-breaker (lower = earlier = wins)

  // For use with std::greater<RankKey> - returns true if this should come AFTER other
  bool operator<(const RankKey& other) const {
    if (value != other.value) {
      return value < other.value;
    }
    return arrivalSeq > other.arrivalSeq;  // Lower arrivalSeq wins ties (comes first)
  }

  bool operator>(const RankKey& other) const {
    if (value != other.value) {
      return value > other.value;
    }
    return arrivalSeq < other.arrivalSeq;  // Lower arrivalSeq wins ties
  }

  bool operator==(const RankKey& other) const {
    return value == other.value && arrivalSeq == other.arrivalSeq;
  }

  bool operator!=(const RankKey& other) const {
    return !(*this == other);
  }

  bool operator<=(const RankKey& other) const {
    return *this < other || *this == other;
  }

  bool operator>=(const RankKey& other) const {
    return *this > other || *this == other;
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
struct RankIndex {
  std::map<RankKey, RankedEntry, std::greater<RankKey>>::iterator rankIter;
  uint64_t cachedRank{UINT64_MAX};  // Cached rank position, invalidated on changes
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
 * TopNGroup - Group of subscribers with same N value.
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
 * Performance stats for PropertyRanking operations.
 * Only collected when statsEnabled() is true (disabled by default).
 */
struct PropertyRankingStats {
  // Track operations
  uint64_t tracksRegistered{0};
  uint64_t tracksRemoved{0};
  uint64_t valueUpdates{0};
  uint64_t fastPathHits{0};    // Updates that didn't cross threshold
  uint64_t slowPathHits{0};    // Updates that required recomputation

  // Selection operations
  uint64_t selectionsTriggered{0};
  uint64_t evictionsTriggered{0};
  uint64_t selfExclusionSkips{0};  // Tracks skipped due to self-exclusion

  // Session operations
  uint64_t sessionsAdded{0};
  uint64_t sessionsRemoved{0};

  // Control flag
  bool enabled{false};
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
      bool forward)>;

  using EvictCallback = std::function<void(
      const moxygen::FullTrackName& ftn,
      std::shared_ptr<moxygen::MoQSession> session)>;

  // Batch notification callback for viewers.
  // Called once per track state change with all affected viewer sessions.
  using BatchSelectCallback = std::function<void(
      const moxygen::FullTrackName& ftn,
      const std::vector<std::pair<std::shared_ptr<moxygen::MoQSession>, bool>>& sessions)>;

  /**
   * Constructor.
   * @param propertyType The property type ID to rank by
   * @param maxDeselected Maximum tracks in deselected queue before eviction
   * @param onBatchSelected Batch callback for viewer notifications (required)
   * @param onSelected Individual callback for publisher notifications
   * @param onEvicted Callback when track is evicted from deselected queue
   */
  PropertyRanking(
      uint64_t propertyType,
      uint64_t maxDeselected,
      BatchSelectCallback onBatchSelected,
      SelectCallback onSelected,
      EvictCallback onEvicted);

  ~PropertyRanking() = default;

  // Non-copyable, non-movable
  PropertyRanking(const PropertyRanking&) = delete;
  PropertyRanking& operator=(const PropertyRanking&) = delete;
  PropertyRanking(PropertyRanking&&) = delete;
  PropertyRanking& operator=(PropertyRanking&&) = delete;

  uint64_t propertyType() const {
    return propertyType_;
  }

  /**
   * Register a track with optional initial value.
   * Called at publish() time.
   * @param ftn The full track name
   * @param initialValue Optional initial property value (defaults to 0)
   * @param publisher The session publishing this track (required)
   */
  void registerTrack(
      const moxygen::FullTrackName& ftn,
      std::optional<uint64_t> initialValue,
      std::weak_ptr<moxygen::MoQSession> publisher);

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
   * If publishedTracks is provided, enables self-exclusion for this session.
   */
  void addSessionToTopNGroup(
      uint64_t maxSelected,
      std::shared_ptr<moxygen::MoQSession> session,
      bool forward,
      std::vector<moxygen::FullTrackName> publishedTracks = {});

  /**
   * Remove a session from a TopNGroup.
   */
  void removeSessionFromTopNGroup(
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
   * Remove TopNGroup (last subscriber with this N left).
   */
  void removeTopNGroup(uint64_t maxSelected);

  /**
   * Check if any TopNGroups remain.
   */
  bool empty() const {
    return topNGroups_.empty();
  }

  /**
   * Get number of registered tracks.
   */
  size_t numTracks() const {
    return tracks_.size();
  }

  /**
   * Get number of TopNGroups.
   */
  size_t numTopNGroups() const {
    return topNGroups_.size();
  }

  // Test accessors
  const std::map<RankKey, RankedEntry, std::greater<RankKey>>& rankedTracks() const {
    return rankedTracks_;
  }

  const TopNGroup* getTopNGroup(uint64_t maxSelected) const {
    auto it = topNGroups_.find(maxSelected);
    return it != topNGroups_.end() ? &it->second : nullptr;
  }

  /**
   * Get performance stats snapshot.
   * Stats are only collected when enabled via enableStats().
   */
  const PropertyRankingStats& stats() const {
    return stats_;
  }

  /**
   * Enable/disable stats collection (disabled by default for performance).
   */
  void enableStats() {
    stats_.enabled = true;
  }

  void disableStats() {
    stats_.enabled = false;
  }

  bool statsEnabled() const {
    return stats_.enabled;
  }

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
   * Recompute all TopNGroups after a rank change.
   */
  void recomputeTopNGroups(
      const moxygen::FullTrackName& ftn,
      std::optional<RankKey> oldKey,
      const RankKey& newKey);

  /**
   * Update selection threshold when TopNGroups change.
   * selectionThreshold_ = max(N) + maxDeselected_
   */
  void updateSelectionThreshold();

  /**
   * Evict oldest track from deselected queue if over limit.
   */
  void trimDeselectedQueue(TopNGroup& topNGroup);

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
  BatchSelectCallback onBatchSelected_;
  SelectCallback onSelected_;
  EvictCallback onEvicted_;

  // Sorted container: key -> RankedEntry (descending order via std::greater)
  std::map<RankKey, RankedEntry, std::greater<RankKey>> rankedTracks_;

  // O(1) lookup: trackName -> iterator into rankedTracks_
  folly::F14FastMap<moxygen::FullTrackName, RankIndex, moxygen::FullTrackName::hash> tracks_;

  // TopNGroups by maxSelected value (N)
  folly::F14FastMap<uint64_t, TopNGroup> topNGroups_;

  // Monotonic sequence for tie-breaking
  uint64_t nextSeq_{0};

  // Cached selection threshold (max N + maxDeselected_)
  uint64_t selectionThreshold_{0};

  // Sorted threshold values for O(log G) crossesThreshold check
  std::vector<uint64_t> sortedThresholds_;

  // Performance stats
  mutable PropertyRankingStats stats_;

  // Rank cache invalidation flag - mutable because cache rebuild is const-safe
  mutable bool rankCacheValid_{false};
};

} // namespace openmoq::moqx
