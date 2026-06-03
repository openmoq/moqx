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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace openmoq::moqx {

/**
 * TrackRank - Entry in the sorted snapshot.
 * Tracks are ranked by property value (descending), with arrival sequence as tie-breaker.
 */
struct TrackRank {
  moxygen::FullTrackName ftn;
  uint64_t propertyValue{0};
  uint64_t arrivalSeq{0};
  std::chrono::steady_clock::time_point lastUpdateTs{};
  moxygen::MoQSession* publisherRaw{nullptr};

  bool operator>(const TrackRank& other) const {
    if (propertyValue != other.propertyValue) {
      return propertyValue > other.propertyValue;
    }
    return arrivalSeq < other.arrivalSeq;
  }

  bool operator<(const TrackRank& other) const {
    return other > *this;
  }

  bool operator==(const TrackRank& other) const {
    return propertyValue == other.propertyValue && arrivalSeq == other.arrivalSeq;
  }
};

/**
 * Snapshot - Immutable sorted vector of top tracks.
 * Shared via shared_ptr for lock-free reads.
 */
using Snapshot = std::vector<TrackRank>;

/**
 * SubscriberFilter - Minimal per-subscriber state.
 * Only stores the N value; uses global snapshot for ranking.
 */
struct SubscriberFilter {
  uint8_t trackFilterN{0};
  bool forward{true};
};

/**
 * SessionSelectionState - Tracks what each session has been notified about.
 * Used by relay to compute selection deltas with self-exclusion.
 */
struct SessionSelectionState {
  uint64_t maxSelected{0};
  bool forward{true};
  folly::F14FastSet<moxygen::FullTrackName, moxygen::FullTrackName::hash> selectedTracks;

  // Cached self-position optimization: position of last self-track in snapshot.
  // If track.position > N + cachedLastSelfPosition, definitely not in top-N non-self.
  // Allows O(1) rejection for most tracks.
  uint8_t cachedLastSelfPosition{0};
  uint64_t cachedSnapshotVersion{0};  // To invalidate cache on snapshot change

  // Whether this session publishes any tracks (used for grouping optimization)
  bool isPublisher{false};
};

/**
 * SimpleTopNTracker - Lock-free top-N tracking for TRACK_FILTER.
 *
 * Design principles (from simple_filter_design.md):
 * 1. Single global data structure per namespace for top-N tracking
 * 2. Lock-free reads during egress (hot path) via atomic shared_ptr
 * 3. Atomic updates on ingest (cold path relative to egress fan-out)
 * 4. Filter evaluation at relay level with self-exclusion
 * 5. Minimal per-subscriber state in tracker; relay owns selection state
 *
 * Key insight: Tracker maintains a global sorted snapshot. On any change,
 * it notifies the relay with old/new snapshots. The relay evaluates
 * per-subscriber selection WITH self-exclusion and fires select/evict.
 *
 * Self-exclusion: If subscriber wants top-3 and their own track is at rank #2,
 * they should receive ranks #1, #3, #4 (skipping self). This is handled at
 * the relay level where publisher info is available per-track.
 */
class SimpleTopNTracker {
public:
  /**
   * Callback when snapshot changes. Relay uses this to recompute per-session selection.
   * @param oldSnapshot Previous snapshot (for diffing)
   * @param newSnapshot New snapshot
   * @param removedTrack If a track was explicitly removed, its FTN (for eviction)
   */
  using SnapshotChangedCallback = std::function<void(
      std::shared_ptr<const Snapshot> oldSnapshot,
      std::shared_ptr<const Snapshot> newSnapshot,
      std::optional<moxygen::FullTrackName> removedTrack
  )>;

  using GetLastActivityFn =
      std::function<std::chrono::steady_clock::time_point(const moxygen::FullTrackName&)>;

  /**
   * @param propertyType       The property type ID to rank by
   * @param idleTimeout        How long a track may be silent before being removed
   * @param sweepThrottle      Minimum interval between idle sweeps
   * @param getLastActivity    Callback to read per-track activity time
   * @param onSnapshotChanged  Callback when snapshot changes (relay evaluates selection)
   */
  SimpleTopNTracker(
      uint64_t propertyType,
      std::chrono::milliseconds idleTimeout,
      std::chrono::milliseconds sweepThrottle,
      GetLastActivityFn getLastActivity,
      SnapshotChangedCallback onSnapshotChanged
  );

  ~SimpleTopNTracker() = default;

  SimpleTopNTracker(const SimpleTopNTracker&) = delete;
  SimpleTopNTracker& operator=(const SimpleTopNTracker&) = delete;
  SimpleTopNTracker(SimpleTopNTracker&&) = delete;
  SimpleTopNTracker& operator=(SimpleTopNTracker&&) = delete;

  uint64_t propertyType() const { return propertyType_; }
  uint8_t maxTracksPerPublisher() const { return maxTracksPerPublisher_.load(std::memory_order_relaxed); }

  /**
   * Register a track. Called at publish() time.
   */
  void registerTrack(
      const moxygen::FullTrackName& ftn,
      std::optional<uint64_t> initialPropertyValue,
      std::shared_ptr<moxygen::MoQSession> publisher
  );

  /**
   * Update track's sort value. Main hot-path entry point.
   */
  void updateSortValue(const moxygen::FullTrackName& ftn, uint64_t value);

  /**
   * Remove track (on PUBLISH_DONE).
   */
  void removeTrack(const moxygen::FullTrackName& ftn);

  /**
   * Add a session with given N value. Triggers snapshot callback for initial selection.
   */
  void addSession(uint64_t maxSelected, std::shared_ptr<moxygen::MoQSession> session, bool forward);

  /**
   * Update the forward flag for a session.
   */
  void updateSessionForward(
      uint64_t maxSelected,
      const std::shared_ptr<moxygen::MoQSession>& session,
      bool forward
  );

  /**
   * Remove a session.
   */
  void removeSession(uint64_t maxSelected, const std::shared_ptr<moxygen::MoQSession>& session);

  /**
   * Sweep idle tracks.
   */
  void sweepIdle();

  /**
   * Load current snapshot (lock-free read for hot path).
   */
  std::shared_ptr<const Snapshot> loadSnapshot() const {
    return std::atomic_load(&snapshot_);
  }

  /**
   * Get publisher for a track (for relay self-exclusion check).
   */
  moxygen::MoQSession* getPublisher(const moxygen::FullTrackName& ftn) const;

  /**
   * Check if a track should be forwarded to a subscriber (without self-exclusion).
   * For self-exclusion aware check, use isInTopNWithSelfExclusion().
   */
  bool shouldForward(
      const moxygen::FullTrackName& ftn,
      uint8_t subscriberN,
      const Snapshot& snapshot
  ) const;

  /**
   * Check if a track is in the subscriber's top-N, with self-exclusion.
   * Uses O(1) fast rejection via cached self-position before falling back to scan.
   * This is the hot-path API for egress filter evaluation.
   *
   * @param ftn Track to check
   * @param session Subscriber session (for self-exclusion)
   * @param n Subscriber's N value
   * @param snapshot Current snapshot
   * @param cachedLastSelfPosition Cached position of subscriber's last self-track
   * @return true if track should be forwarded to this subscriber
   */
  bool isInTopNWithSelfExclusion(
      const moxygen::FullTrackName& ftn,
      const std::shared_ptr<moxygen::MoQSession>& session,
      uint64_t n,
      const Snapshot& snapshot,
      uint8_t cachedLastSelfPosition
  ) const;

  /**
   * Compute top-N tracks for a session, excluding self-published tracks.
   * This is the main API for relay to get per-session selection.
   */
  std::vector<moxygen::FullTrackName> computeTopNForSession(
      const std::shared_ptr<moxygen::MoQSession>& session,
      uint64_t n,
      const Snapshot& snapshot
  ) const;

  /**
   * Fast-path check: can this track be quickly rejected?
   * Uses cached last self-position for O(1) rejection of tracks that
   * definitely aren't in the subscriber's top-N non-self.
   *
   * @param trackPosition Position of track in snapshot (0-indexed)
   * @param n Subscriber's N value
   * @param cachedLastSelfPosition Cached position of last self-track
   * @return true if track MIGHT be in top-N (needs full scan), false if definitely not
   */
  static bool mightBeInTopN(
      size_t trackPosition,
      uint8_t n,
      uint8_t cachedLastSelfPosition
  ) {
    // If track is beyond N + lastSelfPosition, it can't be in top-N non-self
    // even if all self-tracks are above this position
    return trackPosition <= static_cast<size_t>(n) + cachedLastSelfPosition;
  }

  /**
   * Compute cached last self-position for a session in current snapshot.
   * Returns the 0-indexed position of the last self-track, or 0 if no self-tracks.
   */
  uint8_t computeLastSelfPosition(
      const std::shared_ptr<moxygen::MoQSession>& session,
      const Snapshot& snapshot
  ) const;

  /**
   * Flush coalesced value updates: rebuild snapshot and fire callbacks.
   * When flushIntervalMs > 0, skips if called within that interval of the
   * last flush (batching multiple value updates into one rebuild).
   * With flushIntervalMs == 0, always flushes immediately if dirty.
   */
  void flush();

  /**
   * Set the minimum interval between flushes (0 = no throttle, flush every call).
   * Higher values coalesce more updates per rebuild at the cost of latency.
   */
  void setFlushInterval(std::chrono::milliseconds interval) {
    flushInterval_ = interval;
  }

  /**
   * Check if there are pending value updates that need flushing.
   */
  bool isDirty() const { return dirty_.load(std::memory_order_acquire); }

  bool empty() const { return sessions_.empty(); }
  size_t numTracks() const { return trackIndex_.size(); }
  size_t numSessions() const { return sessions_.size(); }
  bool isSessionPublisher(const std::shared_ptr<moxygen::MoQSession>& session) const {
    auto* raw = session.get();
    for (const auto& [ftn, info] : trackIndex_) {
      if (info.publisher.get() == raw) {
        return true;
      }
    }
    return false;
  }
  uint8_t maxN() const { return maxN_.load(std::memory_order_relaxed); }
  uint64_t snapshotVersion() const { return snapshotVersion_.load(std::memory_order_relaxed); }

  // Test accessors
  const Snapshot* getSnapshotPtr() const { return snapshot_.get(); }

  struct SessionInfo {
    uint64_t maxSelected;
    bool forward;
  };

  const folly::F14FastMap<std::shared_ptr<moxygen::MoQSession>, SessionInfo>& sessions() const {
    return sessions_;
  }

private:
  struct TrackInfo {
    uint64_t propertyValue{0};
    uint64_t arrivalSeq{0};
    std::shared_ptr<moxygen::MoQSession> publisher;
  };

  void rebuildSnapshot();
  void updateMaxN();
  void updateMaxTracksPerPublisher();
  uint64_t findRankInSnapshot(const moxygen::FullTrackName& ftn, const Snapshot& snapshot) const;

  uint64_t propertyType_;
  std::chrono::milliseconds idleTimeout_;
  std::chrono::milliseconds sweepThrottle_;
  std::optional<std::chrono::steady_clock::time_point> lastSweepTime_;
  GetLastActivityFn getLastActivity_;
  SnapshotChangedCallback onSnapshotChanged_;

  // Lock-free snapshot for reads
  std::shared_ptr<Snapshot> snapshot_;

  // Track metadata (protected by mutex for writes)
  folly::F14FastMap<moxygen::FullTrackName, TrackInfo, moxygen::FullTrackName::hash> trackIndex_;

  // Session registry
  folly::F14FastMap<std::shared_ptr<moxygen::MoQSession>, SessionInfo> sessions_;

  // Max N across all subscribers
  std::atomic<uint8_t> maxN_{0};

  // Max tracks per publisher (for snapshot size = maxN + maxX)
  std::atomic<uint8_t> maxTracksPerPublisher_{0};

  // Track count per publisher for computing maxTracksPerPublisher
  folly::F14FastMap<moxygen::MoQSession*, size_t> publisherTrackCount_;

  // Arrival sequence counter
  uint64_t nextSeq_{0};

  // Snapshot version for cache invalidation
  std::atomic<uint64_t> snapshotVersion_{0};

  // Dirty flag for coalesced value updates
  std::atomic<bool> dirty_{false};

  // Minimum interval between flushes for coalescing
  std::chrono::milliseconds flushInterval_{0};
  std::chrono::steady_clock::time_point lastFlushTime_{};

  // Mutex for write operations (ingest path)
  mutable std::mutex writeMutex_;
};

/**
 * SimpleTopNRanking - Adapter class matching PropertyRanking interface.
 *
 * This adapter handles:
 * 1. Per-session selection state (what tracks each session has been notified about)
 * 2. Self-exclusion during selection (skip tracks published by the subscriber)
 * 3. Firing select/evict callbacks based on selection deltas
 *
 * The underlying SimpleTopNTracker just maintains the global sorted snapshot.
 */
class SimpleTopNRanking {
public:
  using SelectCallback = std::function<void(
      const moxygen::FullTrackName& ftn,
      std::shared_ptr<moxygen::MoQSession> session,
      bool forward
  )>;

  using EvictCallback = std::function<
      void(const moxygen::FullTrackName& ftn, std::shared_ptr<moxygen::MoQSession> session)>;

  using BatchSelectCallback = std::function<void(
      const moxygen::FullTrackName& ftn,
      const std::vector<std::pair<std::shared_ptr<moxygen::MoQSession>, bool>>& sessions
  )>;

  using GetLastActivityFn = SimpleTopNTracker::GetLastActivityFn;

  SimpleTopNRanking(
      uint64_t propertyType,
      uint64_t maxDeselected, // Ignored in simple design
      std::chrono::milliseconds idleTimeout,
      std::chrono::milliseconds sweepThrottle,
      GetLastActivityFn getLastActivity,
      BatchSelectCallback onBatchSelected,
      SelectCallback onSelected,
      EvictCallback onEvicted
  );

  ~SimpleTopNRanking() = default;

  uint64_t propertyType() const { return tracker_.propertyType(); }

  void registerTrack(
      const moxygen::FullTrackName& ftn,
      std::optional<uint64_t> initialPropertyValue,
      std::shared_ptr<moxygen::MoQSession> publisher
  );

  void updateSortValue(const moxygen::FullTrackName& ftn, uint64_t value);

  void removeTrack(const moxygen::FullTrackName& ftn);

  void addSessionToTopNGroup(
      uint64_t maxSelected,
      std::shared_ptr<moxygen::MoQSession> session,
      bool forward
  );

  void updateSessionForward(
      uint64_t maxSelected,
      const std::shared_ptr<moxygen::MoQSession>& session,
      bool forward
  );

  void removeSessionFromTopNGroup(
      uint64_t maxSelected,
      const std::shared_ptr<moxygen::MoQSession>& session
  );

  void sweepIdle() { tracker_.sweepIdle(); }

  /**
   * Flush coalesced value updates. Call after updateSortValue to process
   * pending changes; respects flush interval for coalescing.
   */
  void flush() { tracker_.flush(); }

  void setFlushInterval(std::chrono::milliseconds interval) {
    tracker_.setFlushInterval(interval);
  }

  bool empty() const { return tracker_.empty(); }
  size_t numTracks() const { return tracker_.numTracks(); }

  // For compatibility with tests
  struct TopNGroupCompat {
    uint64_t maxSelected{0};
    folly::F14FastMap<std::shared_ptr<moxygen::MoQSession>, SubscriberFilter> sessions;
  };

  const TopNGroupCompat* getTopNGroup(uint64_t maxSelected) const;
  size_t numTopNGroups() const { return topNGroups_.size(); }

private:
  /**
   * Recompute selection for a session and fire callbacks for deltas.
   * Self-exclusion: skips tracks where publisher == session.
   */
  void recomputeSessionSelection(
      const std::shared_ptr<moxygen::MoQSession>& session,
      SessionSelectionState& state,
      const Snapshot& snapshot,
      std::optional<moxygen::FullTrackName> removedTrack = std::nullopt
  );

  /**
   * Called by tracker when snapshot changes.
   */
  void onSnapshotChanged(
      std::shared_ptr<const Snapshot> oldSnapshot,
      std::shared_ptr<const Snapshot> newSnapshot,
      std::optional<moxygen::FullTrackName> removedTrack
  );

  // Check if the top-N boundary changed between old and new snapshots.
  // Returns false if the first N+X entries are identical (no recomputation needed).
  static bool topNBoundaryChanged(
      const Snapshot& oldSnap,
      const Snapshot& newSnap,
      size_t boundarySize
  );

  SimpleTopNTracker tracker_;

  // Per-session selection state (tracks what each session has been notified about)
  folly::F14FastMap<std::shared_ptr<moxygen::MoQSession>, SessionSelectionState> sessionStates_;

  // Pure subscribers grouped by N value — all share the same selection (no self-exclusion).
  // Key: N value, Value: shared selection state + list of sessions
  struct PureSubscriberGroup {
    folly::F14FastSet<moxygen::FullTrackName, moxygen::FullTrackName::hash> selectedTracks;
    std::vector<std::shared_ptr<moxygen::MoQSession>> sessions;
    std::vector<bool> forwards;
  };
  folly::F14FastMap<uint64_t, PureSubscriberGroup> pureSubscriberGroups_;

  // Compatibility layer for TopNGroup-style API
  mutable folly::F14FastMap<uint64_t, TopNGroupCompat> topNGroups_;

  // Callbacks
  BatchSelectCallback onBatchSelected_;
  SelectCallback onSelected_;
  EvictCallback onEvicted_;

  mutable std::mutex stateMutex_;
};

} // namespace openmoq::moqx
