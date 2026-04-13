/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/PropertyRanking.h"

#include <folly/logging/xlog.h>

namespace openmoq::moqx {

PropertyRanking::PropertyRanking(
    uint64_t propertyType,
    uint64_t maxDeselected,
    std::chrono::milliseconds idleTimeout,
    GetLastActivityFn getLastActivity,
    BatchSelectCallback onBatchSelected,
    SelectCallback onSelected,
    EvictCallback onEvicted
)
    : propertyType_(propertyType), maxDeselected_(maxDeselected), idleTimeout_(idleTimeout),
      getLastActivity_(std::move(getLastActivity)), onBatchSelected_(std::move(onBatchSelected)),
      onSelected_(std::move(onSelected)), onEvicted_(std::move(onEvicted)) {}

/**
 * Register a new track with an optional initial property value.
 *
 * Algorithm overview:
 * 1. Insert into sorted map (O(log T))
 * 2. Compute rank using previous track's cached rank (O(1))
 * 3. Incrementally update ranks for tracks shifted down (O(T - rank))
 * 4. Two-phase TopNGroup update:
 *    - Phase 1: Mark selected in applicable groups, collect demotion positions
 *    - Phase 2: Single traversal to demote tracks pushed out of top-N
 *
 * Total complexity: O(log T + T - rank + G log G + N) where T=tracks, G=groups, N=max(N)
 */
void PropertyRanking::registerTrack(
    const moxygen::FullTrackName& ftn,
    std::optional<uint64_t> initialValue,
    std::weak_ptr<moxygen::MoQSession> publisher
) {
  if (trackIndexByName_.find(ftn) != trackIndexByName_.end()) {
    XLOG(WARN) << "Track already registered: " << ftn;
    return;
  }

  // --- Step 1: Ensure cache validity for O(1) rank computation ---
  rebuildRankCacheIfNeeded();

  // --- Step 2: Insert into sorted map and update publisher track count ---
  uint64_t value = initialValue.value_or(0);
  RankKey key{value, nextSeq_++};

  // Increment publisher track count for O(1) isPublisher() lookup
  if (auto pubSession = publisher.lock()) {
    auto& entry = publisherTrackCount_[pubSession.get()];
    entry.session = pubSession; // Store weak_ptr for ABA validation
    entry.trackCount++;
  }

  auto [iter, inserted] =
      rankedTracks_.emplace(key, RankedEntry{.ftn = ftn, .publisher = std::move(publisher)});

  // --- Step 3: Compute rank in O(1) using previous track's cached rank ---
  // Since map is sorted descending, the previous iterator has the next-lower rank.
  uint64_t rank;
  if (iter == rankedTracks_.begin()) {
    // Highest value track gets rank 0
    rank = 0;
  } else {
    auto prevIt = iter;
    --prevIt;
    auto prevIndexIt = trackIndexByName_.find(prevIt->second.ftn);
    XCHECK(prevIndexIt != trackIndexByName_.end()) << "Previous track must exist in index";

    // Handle sentinel: if previous track has UINT64_MAX (lazy partial cache),
    // fall back to O(rank) distance computation for accurate rank.
    if (prevIndexIt->second.cachedRank == UINT64_MAX) {
      rank = static_cast<uint64_t>(std::distance(rankedTracks_.begin(), iter));
    } else {
      rank = prevIndexIt->second.cachedRank + 1;
    }
  }

  trackIndexByName_[ftn] = RankIndex{.rankIter = iter, .cachedRank = rank};

  // --- Step 4: Increment cached ranks for all tracks shifted down ---
  // Tracks after insertion point had their rank increase by 1.
  // Skip tracks with sentinel value (UINT64_MAX from lazy partial cache).
  auto nextIt = iter;
  ++nextIt;
  for (; nextIt != rankedTracks_.end(); ++nextIt) {
    auto indexIt = trackIndexByName_.find(nextIt->second.ftn);
    if (indexIt != trackIndexByName_.end() && indexIt->second.cachedRank != UINT64_MAX) {
      indexIt->second.cachedRank++;
    }
  }
  // Cache remains valid - no full rebuild needed

  XLOG(DBG4) << "Registered track " << ftn << " with value " << value << " at rank " << rank;

  // --- Step 5: Two-phase TopNGroup update ---
  // Phase 1: Mark track selected in applicable groups, collect demotion positions.
  // For each group where rank < N, the new track enters top-N and pushes
  // the track at rank N-1 down to rank N (out of top-N).
  std::vector<std::pair<uint64_t, TopNGroup*>> demotions; // (position n, group)

  for (auto& [n, topNGroup] : topNGroups_) {
    if (rank < n) {
      topNGroup.trackStates[ftn] = TrackState::Selected;
      notifyTrackSelected(ftn, topNGroup);

      // Queue demotion at position N (the first rank outside top-N).
      // Only fires when numTracks > N (otherwise no track to demote).
      demotions.emplace_back(n, &topNGroup);
    }
    // Tracks outside top-N are not added to deselected queue on registration;
    // they only enter the queue when falling out of an already-selected position.
  }

  // Phase 2: Single traversal to demote tracks at collected positions.
  // By sorting demotion positions, we process all demotions in one pass.
  if (!demotions.empty()) {
    IterationGuard guard(*this);

    // Sort by position for single-pass processing
    std::sort(demotions.begin(), demotions.end(), [](const auto& a, const auto& b) {
      return a.first < b.first;
    });

    size_t demIdx = 0;
    uint64_t count = 0;
    for (auto& [key, rankedEntry] : rankedTracks_) {
      // Process all demotions at current position
      while (demIdx < demotions.size() && count == demotions[demIdx].first) {
        demoteTrackInGroup(*demotions[demIdx].second, rankedEntry.ftn, count);
        demIdx++;
      }
      if (demIdx >= demotions.size()) {
        break; // All demotions processed
      }
      count++;
    }
  }

  // --- Step 6: Handle self-exclusion for publisher-subscribers ---
  // If the track publisher is already subscribed in some TopNGroup, this new
  // track becomes a self-track for that session. Reconcile to exclude it.
  auto pubSession = publisher.lock();
  if (pubSession) {
    reconcilePublisherInAllGroups(pubSession);
  }
}

/**
 * Update a track's sort value. Main hot-path entry point.
 *
 * Algorithm overview:
 * 1. Early exit if value unchanged
 * 2. Re-insert into sorted map at new position (O(log T))
 * 3. Compute new rank via std::distance (O(newRank))
 * 4. Incrementally update cached ranks for affected range (O(|movement|))
 * 5. Fast path exit if no threshold crossed
 * 6. Slow path: recompute TopNGroup states
 *
 * The fast path (step 5) avoids expensive group updates when rank changes
 * don't affect any subscriber's top-N selection.
 */
void PropertyRanking::updateSortValue(const moxygen::FullTrackName& ftn, uint64_t value) {
  auto it = trackIndexByName_.find(ftn);
  if (it == trackIndexByName_.end()) {
    XLOG(DBG4) << "updateSortValue: track not registered " << ftn;
    return;
  }

  auto& entry = it->second;
  RankKey oldKey = entry.rankIter->first;

  // --- Step 1: Early exit if no change ---
  if (oldKey.value == value) {
    return;
  }

  // --- Step 2: Re-insert at new position in sorted map ---
  RankKey newKey{value, oldKey.arrivalSeq}; // Preserve arrival sequence for tie-breaking

  rebuildRankCacheIfNeeded();
  uint64_t oldRank = entry.cachedRank;

  // Erase and re-insert: O(log T) each
  auto rankedEntry = std::move(entry.rankIter->second);
  rankedTracks_.erase(entry.rankIter);
  auto [newIter, _] = rankedTracks_.emplace(newKey, std::move(rankedEntry));
  entry.rankIter = newIter;

  // --- Step 3: Compute new rank ---
  // O(newRank) for bidirectional iterator. Acceptable because top-ranked tracks
  // (where we care most about performance) have small newRank values.
  uint64_t newRank = static_cast<uint64_t>(std::distance(rankedTracks_.begin(), newIter));
  entry.cachedRank = newRank;

  // --- Step 4: Incrementally update cached ranks for affected tracks ---
  // Only tracks between old and new positions are affected: O(|oldRank - newRank|).
  //
  // Special case: if oldRank == UINT64_MAX (sentinel from lazy partial cache),
  // the track was outside the selection threshold. We only need to update
  // tracks up to selectionThreshold_ since tracks beyond don't affect selection.
  uint64_t effectiveOldRank =
      (oldRank == UINT64_MAX)
          ? std::min(static_cast<uint64_t>(rankedTracks_.size()), selectionThreshold_)
          : oldRank;

  if (newRank < effectiveOldRank) {
    // Track moved UP (higher value → lower rank number).
    // Tracks in range (newRank, effectiveOldRank] shift DOWN by 1 (their rank increases).
    // Example: track moves from rank 5 to rank 2
    //   Before: [0, 1, 2, 3, 4, T, 6, ...]  (T was at rank 5)
    //   After:  [0, 1, T, 2, 3, 4, 6, ...]  (tracks 2,3,4 shift to 3,4,5)
    auto rankedIt = newIter;
    ++rankedIt; // Start after the moved track
    for (uint64_t r = newRank + 1; r <= effectiveOldRank && rankedIt != rankedTracks_.end();
         ++r, ++rankedIt) {
      auto indexIt = trackIndexByName_.find(rankedIt->second.ftn);
      if (indexIt != trackIndexByName_.end()) {
        indexIt->second.cachedRank = r;
      }
    }
  } else if (newRank > effectiveOldRank && oldRank != UINT64_MAX) {
    // Track moved DOWN (lower value → higher rank number).
    // Tracks in range [oldRank, newRank) shift UP by 1 (their rank decreases).
    // Note: Skip if oldRank was sentinel - no accurate prior position to update from.
    // Example: track moves from rank 2 to rank 5
    //   Before: [0, 1, T, 3, 4, 5, 6, ...]  (T was at rank 2)
    //   After:  [0, 1, 2, 3, 4, T, 6, ...]  (tracks 3,4,5 shift to 2,3,4)
    auto rankedIt = newIter;
    for (uint64_t r = newRank; r > oldRank && rankedIt != rankedTracks_.begin();) {
      --rankedIt;
      --r;
      auto indexIt = trackIndexByName_.find(rankedIt->second.ftn);
      if (indexIt != trackIndexByName_.end()) {
        indexIt->second.cachedRank = r;
      }
    }
  }
  // Cache remains valid - no full rebuild needed

  // --- Step 5: Fast path - no threshold crossed ---
  if (!crossesThreshold(oldRank, newRank)) {
    XLOG(DBG5) << "updateSortValue fast path: " << ftn << " value=" << value << " rank " << oldRank
               << " -> " << newRank << " (no threshold crossed)";
    return;
  }

  // Opportunistic idle sweep: a value change is worth a quick idle check
  sweepIdle();

  // --- Step 6: Slow path - threshold crossed, update TopNGroups ---
  XLOG(DBG4) << "updateSortValue slow path: " << ftn << " value=" << value << " rank " << oldRank
             << " -> " << newRank << " (threshold crossed)";
  recomputeTopNGroups(ftn, oldRank, newRank);
}

/**
 * Remove a track (typically on PUBLISH_DONE).
 *
 * Algorithm overview:
 * 1. Remove from sorted map and index (O(log T))
 * 2. Decrement cached ranks for tracks shifted up (O(T - oldRank))
 * 3. For each TopNGroup where track was selected, promote a replacement:
 *    - Try deselected queue first (O(1))
 *    - Fallback: scan ranked list for first non-selected track (O(N))
 */
void PropertyRanking::removeTrack(const moxygen::FullTrackName& ftn) {
  auto it = trackIndexByName_.find(ftn);
  if (it == trackIndexByName_.end()) {
    return;
  }

  // --- Step 1: Ensure cache is valid and capture position ---
  rebuildRankCacheIfNeeded();

  auto& entry = it->second;
  uint64_t oldRank = entry.cachedRank;

  // Save iterator to next track before erasing (remains valid per std::map guarantees)
  auto nextIt = entry.rankIter;
  ++nextIt;

  // Decrement publisher track count before erasing
  if (auto pubSession = entry.rankIter->second.publisher.lock()) {
    auto countIt = publisherTrackCount_.find(pubSession.get());
    if (countIt != publisherTrackCount_.end()) {
      if (--countIt->second.trackCount == 0) {
        publisherTrackCount_.erase(countIt);
      }
    }
  }

  // --- Step 2: Remove from data structures ---
  rankedTracks_.erase(entry.rankIter);
  trackIndexByName_.erase(it);

  // --- Step 3: Decrement cached ranks for tracks shifted up ---
  // Tracks after removal point move up by 1 (their rank decreases).
  // Skip tracks with sentinel value (UINT64_MAX from lazy partial cache).
  for (; nextIt != rankedTracks_.end(); ++nextIt) {
    auto indexIt = trackIndexByName_.find(nextIt->second.ftn);
    if (indexIt != trackIndexByName_.end() && indexIt->second.cachedRank != UINT64_MAX) {
      indexIt->second.cachedRank--;
    }
  }
  // Cache remains valid - no full rebuild needed

  XLOG(DBG4) << "Removed track " << ftn << " from rank " << oldRank;

  // --- Step 4: Update TopNGroups and promote replacements ---
  for (auto& [n, topNGroup] : topNGroups_) {
    auto stateIt = topNGroup.trackStates.find(ftn);
    if (stateIt == topNGroup.trackStates.end()) {
      continue;
    }

    bool wasSelected = stateIt->second == TrackState::Selected;
    topNGroup.trackStates.erase(stateIt);

    // Always remove from deselected queue if present
    removeFromDeselectedQueue(topNGroup, ftn);

    if (wasSelected) {
      // Need to promote a replacement into top-N
      IterationGuard guard(*this);

      // Try deselected queue first for O(1) promotion
      if (!topNGroup.deselectedQueue.empty()) {
        auto promoted = topNGroup.deselectedQueue.front();
        topNGroup.deselectedQueue.pop_front();
        XLOG(DBG4) << "removeTrack: promoting " << promoted
                   << " from deselected queue for TopNGroup maxSelected=" << n;
        topNGroup.trackStates[promoted] = TrackState::Selected;
        notifyTrackSelected(promoted, topNGroup);
      } else {
        // Fallback: deselected queue empty (e.g., first removal after registration
        // when no tracks have been demoted yet). Scan for first non-selected track.
        for (auto& [key, rankedEntry] : rankedTracks_) {
          auto stIt = topNGroup.trackStates.find(rankedEntry.ftn);
          if (stIt != topNGroup.trackStates.end() && stIt->second == TrackState::Selected) {
            continue; // Already selected, skip
          }
          // Found first non-selected track - promote it
          topNGroup.trackStates[rankedEntry.ftn] = TrackState::Selected;
          notifyTrackSelected(rankedEntry.ftn, topNGroup);
          break;
        }
      }
    }
  }
}

TopNGroup& PropertyRanking::getOrCreateTopNGroup(uint64_t maxSelected) {
  auto [it, inserted] = topNGroups_.try_emplace(maxSelected);
  if (inserted) {
    XLOG(DBG4) << "Created new TopNGroup with maxSelected=" << maxSelected;
    it->second.maxSelected = maxSelected;

    // Capture old threshold before updating
    uint64_t oldThreshold = selectionThreshold_;
    updateSelectionThreshold();

    // If threshold increased, tracks that had UINT64_MAX sentinel may now need
    // accurate ranks. Invalidate cache to force rebuild on next access.
    if (selectionThreshold_ > oldThreshold) {
      rankCacheValid_ = false;
    }

    uint64_t rank = 0;
    for (auto& [key, entry] : rankedTracks_) {
      if (rank < maxSelected) {
        it->second.trackStates[entry.ftn] = TrackState::Selected;
      }
      rank++;
    }
  }
  return it->second;
}

void PropertyRanking::addSessionToTopNGroup(
    uint64_t maxSelected,
    std::shared_ptr<moxygen::MoQSession> session,
    bool forward
) {
  XCHECK(!iteratingSessions_) << "Cannot add session while iterating";
  XLOG(DBG4) << "addSessionToTopNGroup: maxSelected=" << maxSelected << " forward=" << forward;

  auto& topNGroup = getOrCreateTopNGroup(maxSelected);
  SessionInfo info;
  info.forward = forward;

  topNGroup.sessions[session] = std::move(info);
  auto& sessionInfo = topNGroup.sessions[session];

  // Check if this session is a publisher (owns any tracks in rankedTracks_).
  // Self-exclusion is automatic based on RankedEntry::publisher.
  if (isPublisher(session)) {
    // reconcilePublisherSelection handles initial delivery for publishers:
    // it fires onSelected_ for each track in the personal top-N and
    // populates selectedTracks for future delta computation.
    reconcilePublisherSelection(sessionInfo, maxSelected, session);
  } else if (onSelected_) {
    // Notify viewer of all currently-selected tracks.
    uint64_t rank = 0;
    for (auto& [key, entry] : rankedTracks_) {
      if (rank >= maxSelected) {
        break;
      }
      onSelected_(entry.ftn, session, sessionInfo.forward);
      rank++;
    }
  }
}

void PropertyRanking::updateSessionForward(
    uint64_t maxSelected,
    const std::shared_ptr<moxygen::MoQSession>& session,
    bool forward
) {
  auto it = topNGroups_.find(maxSelected);
  if (it == topNGroups_.end()) {
    XLOG(WARN) << "updateSessionForward: TopNGroup not found for maxSelected=" << maxSelected;
    return;
  }

  auto sessionIt = it->second.sessions.find(session);
  if (sessionIt == it->second.sessions.end()) {
    XLOG(WARN) << "updateSessionForward: session not found in TopNGroup maxSelected="
               << maxSelected;
    return;
  }

  XLOG(DBG4) << "updateSessionForward: maxSelected=" << maxSelected
             << " forward=" << sessionIt->second.forward << " -> " << forward;
  sessionIt->second.forward = forward;
}

void PropertyRanking::removeSessionFromTopNGroup(
    uint64_t maxSelected,
    const std::shared_ptr<moxygen::MoQSession>& session
) {
  XCHECK(!iteratingSessions_) << "Cannot remove session while iterating";

  auto it = topNGroups_.find(maxSelected);
  if (it == topNGroups_.end()) {
    XLOG(WARN) << "removeSessionFromTopNGroup: TopNGroup not found for maxSelected=" << maxSelected;
    return;
  }

  it->second.sessions.erase(session);

  if (it->second.sessions.empty()) {
    removeTopNGroup(maxSelected);
  }
}

void PropertyRanking::removeTopNGroup(uint64_t maxSelected) {
  XLOG(DBG4) << "removeTopNGroup: maxSelected=" << maxSelected;
  topNGroups_.erase(maxSelected);
  updateSelectionThreshold();
}

uint64_t PropertyRanking::getRank(const RankKey& key) const {
  auto it = rankedTracks_.find(key);
  if (it == rankedTracks_.end()) {
    return UINT64_MAX;
  }
  rebuildRankCacheIfNeeded();
  return trackIndexByName_.find(it->second.ftn)->second.cachedRank;
}

/**
 * Rebuild rank cache if invalid.
 *
 * Complexity: O(min(numTracks, selectionThreshold_)) with lazy partial caching.
 *
 * Only called when cache is invalid (e.g., after direct map manipulation in tests).
 * Normal operations use incremental updates:
 * - registerTrack: O(1) rank lookup via iterator--, O(numTracks - rank) updates
 * - removeTrack: O(numTracks - oldRank) updates
 * - updateSortValue: O(newRank) + O(|movement|) updates
 *
 * Lazy partial cache optimization:
 * Only computes ranks up to selectionThreshold_ (max(N) + maxDeselected).
 * Tracks beyond this threshold get cachedRank = UINT64_MAX (sentinel).
 * Rationale: tracks past the threshold don't affect any top-N selection or
 * deselected queue, so their exact rank doesn't matter for correctness.
 *
 * When selectionThreshold_ increases (new TopNGroup with larger N), the cache
 * is invalidated in getOrCreateTopNGroup() to ensure tracks in the expanded
 * range get accurate ranks on next rebuild.
 */
void PropertyRanking::rebuildRankCacheIfNeeded() const {
  if (rankCacheValid_) {
    return;
  }

  // Traverse sorted map and assign sequential ranks (0 = highest value).
  // trackIndexByName_ is mutable to allow this lazy cache update in const context.
  uint64_t rank = 0;
  for (const auto& [key, entry] : rankedTracks_) {
    auto it = trackIndexByName_.find(entry.ftn);
    if (it != trackIndexByName_.end()) {
      // Lazy partial cache: stop computing exact ranks past selectionThreshold_.
      // These tracks are outside all top-N groups and deselected queues.
      if (rank < selectionThreshold_ || selectionThreshold_ == 0) {
        it->second.cachedRank = rank;
      } else {
        // Sentinel value indicates "rank >= selectionThreshold_"
        it->second.cachedRank = UINT64_MAX;
      }
    }
    rank++;
  }
  rankCacheValid_ = true;
}

/**
 * Check if a rank change crosses any TopNGroup threshold.
 *
 * A threshold is crossed when the track enters or exits some group's top-N.
 * For group with N=5, threshold is at rank 5 (ranks 0-4 are in, 5+ are out).
 *
 * Algorithm: O(log G) using sorted thresholds
 * 1. Fast exit if both ranks outside all selection regions
 * 2. Check if track crossed into/out of selection region entirely
 * 3. Binary search for any threshold between old and new rank
 *
 * Example: groups with N=3 and N=7, track moves from rank 5 to rank 2
 *   - sortedThresholds_ = [3, 7]
 *   - minRank=2, maxRank=5
 *   - upper_bound(2) finds 3, and 3 <= 5, so threshold crossed (N=3)
 *
 * Sentinel handling: if oldRank == UINT64_MAX (lazy partial cache sentinel),
 * it correctly represents "outside all selection regions" since
 * UINT64_MAX >= selectionThreshold_ is always true.
 */
bool PropertyRanking::crossesThreshold(uint64_t oldRank, uint64_t newRank) const {
  if (topNGroups_.empty()) {
    return false;
  }

  // --- Fast exit: both ranks outside all selection regions ---
  // selectionThreshold_ = max(N) + maxDeselected_, so ranks >= threshold
  // are outside all groups and their deselected queues.
  if (oldRank >= selectionThreshold_ && newRank >= selectionThreshold_) {
    return false;
  }
  // NOTE: We intentionally do NOT fast-exit when both ranks are below selectionThreshold_.
  // Even though both are within the selection region, a specific N boundary might still
  // be crossed (e.g., moving from rank 5 to rank 2 crosses N=3 but not N=7).

  uint64_t minRank = std::min(oldRank, newRank);
  uint64_t maxRank = std::max(oldRank, newRank);

  // --- Check if track crossed into/out of selection region entirely ---
  bool oldInAnyTopN = oldRank < selectionThreshold_;
  bool newInAnyTopN = newRank < selectionThreshold_;
  if (oldInAnyTopN != newInAnyTopN) {
    return true;
  }

  // --- O(log G) binary search for threshold in (minRank, maxRank] ---
  // upper_bound gives first threshold > minRank; check if it's <= maxRank.
  auto it = std::upper_bound(sortedThresholds_.begin(), sortedThresholds_.end(), minRank);
  if (it != sortedThresholds_.end() && *it <= maxRank) {
    return true;
  }

  return false;
}

/**
 * Recompute TopNGroup states after a track crosses selection thresholds.
 *
 * Two-phase algorithm achieving O(G log G + N) instead of naive O(G * N):
 *
 * Phase 1: Iterate groups O(G)
 *   - Update moved track's state (Selected ↔ Deselected)
 *   - Collect boundary operations (positions where other tracks need demotion/promotion)
 *
 * Phase 2: Single traversal of rankedTracks_ O(N)
 *   - Sort operations by position O(G log G)
 *   - Process all demotions/promotions in one pass
 *
 * Why two phases? When a track enters top-N, it pushes another track out.
 * We need to find the track at position N (the boundary). Rather than
 * traversing rankedTracks_ once per group, we collect all boundary positions
 * and handle them in a single traversal.
 *
 * Example: track moves from rank 8 to rank 2, groups N=3, N=5, N=7
 *   - Track enters N=3, N=5, N=7 → demote tracks at positions 3, 5, 7
 *   - Phase 2: traverse once, demote at positions 3, 5, 7
 */
void PropertyRanking::recomputeTopNGroups(
    const moxygen::FullTrackName& ftn,
    uint64_t oldRank,
    uint64_t newRank
) {
  XLOG(DBG4) << "recomputeTopNGroups: " << ftn << " moved from rank " << oldRank << " to "
             << newRank;

  IterationGuard guard(*this);

  // --- Phase 1: Update state, notify sessions, collect boundary operations ---
  enum class Action { Demote, Promote };
  std::vector<std::tuple<uint64_t, TopNGroup*, Action>> boundaryOps;

  for (auto& [n, topNGroup] : topNGroups_) {
    bool wasInTopN = oldRank < n;
    bool nowInTopN = newRank < n;

    // Update shared track state (governs viewer selection and the
    // deselected queue for cheap reselection).
    if (wasInTopN != nowInTopN) {
      if (nowInTopN) {
        // Track entered top-N: mark selected, remove from deselected queue if present
        topNGroup.trackStates[ftn] = TrackState::Selected;
        removeFromDeselectedQueue(topNGroup, ftn);
      } else {
        // Track fell out of top-N: mark deselected, add to queue
        topNGroup.trackStates[ftn] = TrackState::Deselected;
        topNGroup.deselectedQueue.push_back(ftn);
        trimDeselectedQueue(topNGroup);
      }
    }

    // Viewers get a batch notification when ftn enters the shared top-N.
    // Publishers are reconciled individually: the waterline is key-based and
    // invariant to self-track rank changes, so we only reconcile when ftn is
    // non-self (a non-self move is the only thing that can shift the waterline).
    std::vector<std::pair<std::shared_ptr<moxygen::MoQSession>, bool>> viewerBatch;
    for (auto& [session, info] : topNGroup.sessions) {
      if (isPublisher(session)) {
        if (!isSelfTrack(ftn, session)) {
          reconcilePublisherSelection(info, n, session);
        }
      } else if (!wasInTopN && nowInTopN) {
        viewerBatch.emplace_back(session, info.forward);
      }
    }
    if (!viewerBatch.empty() && onBatchSelected_) {
      onBatchSelected_(ftn, viewerBatch);
    }

    // Queue boundary operations for other tracks affected by this move
    if (!wasInTopN && nowInTopN) {
      // Track entered top-N → demote track at position N (pushed out of top-N)
      boundaryOps.emplace_back(n, &topNGroup, Action::Demote);
    }

    if (wasInTopN && !nowInTopN) {
      // Track fell out of top-N → promote track at position N-1 (now enters top-N)
      boundaryOps.emplace_back(n - 1, &topNGroup, Action::Promote);
    }
  }

  // --- Phase 2: Single traversal to handle all boundary operations ---
  if (boundaryOps.empty()) {
    return;
  }

  // Sort by position for single-pass processing
  std::sort(boundaryOps.begin(), boundaryOps.end(), [](const auto& a, const auto& b) {
    return std::get<0>(a) < std::get<0>(b);
  });

  size_t opIdx = 0;
  uint64_t count = 0;
  for (auto& [key, rankedEntry] : rankedTracks_) {
    // Process all operations at current position
    while (opIdx < boundaryOps.size() && count == std::get<0>(boundaryOps[opIdx])) {
      auto* group = std::get<1>(boundaryOps[opIdx]);
      Action action = std::get<2>(boundaryOps[opIdx]);

      if (action == Action::Demote) {
        demoteTrackInGroup(*group, rankedEntry.ftn, count);
      } else {
        promoteTrackInGroup(*group, rankedEntry.ftn, count);
      }
      opIdx++;
    }

    if (opIdx >= boundaryOps.size()) {
      break; // All operations processed
    }
    count++;
  }
}

void PropertyRanking::updateSelectionThreshold() {
  uint64_t maxN = 0;
  sortedThresholds_.clear();
  sortedThresholds_.reserve(topNGroups_.size());
  for (const auto& [n, topNGroup] : topNGroups_) {
    maxN = std::max(maxN, n);
    sortedThresholds_.push_back(n);
  }
  std::sort(sortedThresholds_.begin(), sortedThresholds_.end());
  selectionThreshold_ = maxN + maxDeselected_;
}

void PropertyRanking::trimDeselectedQueue(TopNGroup& topNGroup) {
  if (topNGroup.deselectedQueue.size() <= maxDeselected_) {
    return;
  }

  // Guard once for all evictions rather than per-iteration
  IterationGuard guard(*this);

  // Log warning once when queue overflows - indicates high track churn
  XLOG(WARN) << "[PropertyRanking] Deselected queue overflow: " << topNGroup.deselectedQueue.size()
             << " tracks queued, maxDeselected=" << maxDeselected_
             << ". Consider increasing maxDeselected if this persists.";

  while (topNGroup.deselectedQueue.size() > maxDeselected_) {
    auto evicted = topNGroup.deselectedQueue.front();
    topNGroup.deselectedQueue.pop_front();
    topNGroup.trackStates.erase(evicted);

    XLOG(DBG4) << "[PropertyRanking] Evicting track " << evicted
               << " from TopNGroup maxSelected=" << topNGroup.maxSelected
               << " (deselectedQueue exceeded maxDeselected=" << maxDeselected_ << ")";

    if (onEvicted_) {
      for (const auto& [session, info] : topNGroup.sessions) {
        onEvicted_(evicted, session);
      }
    }
  }
}

// O(maxDeselected_) linear scan to remove ftn from the deselected queue.
void PropertyRanking::removeFromDeselectedQueue(
    TopNGroup& group,
    const moxygen::FullTrackName& ftn
) {
  auto& dq = group.deselectedQueue;
  dq.erase(std::remove(dq.begin(), dq.end(), ftn), dq.end());
}

void PropertyRanking::demoteTrackInGroup(
    TopNGroup& group,
    const moxygen::FullTrackName& ftn,
    uint64_t rank
) {
  auto stIt = group.trackStates.find(ftn);
  if (stIt != group.trackStates.end() && stIt->second == TrackState::Selected) {
    XLOG(DBG4) << "demoteTrackInGroup: demoting " << ftn << " at rank " << rank
               << " to deselected queue";
    stIt->second = TrackState::Deselected;
    group.deselectedQueue.push_back(ftn);
    trimDeselectedQueue(group);
  }
}

void PropertyRanking::promoteTrackInGroup(
    TopNGroup& group,
    const moxygen::FullTrackName& ftn,
    uint64_t rank
) {
  auto stIt = group.trackStates.find(ftn);
  if (stIt == group.trackStates.end() || stIt->second != TrackState::Selected) {
    XLOG(DBG4) << "promoteTrackInGroup: promoting " << ftn << " at rank " << rank << " into top-N";
    group.trackStates[ftn] = TrackState::Selected;
    removeFromDeselectedQueue(group, ftn);
    notifyTrackSelected(ftn, group);
  }
}

void PropertyRanking::promoteNextAvailableTrack(
    TopNGroup& group,
    const moxygen::FullTrackName& excludeFtn
) {
  // Find the highest-ranked non-selected track and promote it.
  // Scan rankedTracks_ from the top; skip Selected tracks and excludeFtn.
  IterationGuard guard(*this);
  for (const auto& [key, rankedEntry] : rankedTracks_) {
    if (rankedEntry.ftn == excludeFtn) {
      continue;
    }
    auto stIt = group.trackStates.find(rankedEntry.ftn);
    bool isSelected = stIt != group.trackStates.end() && stIt->second == TrackState::Selected;
    if (!isSelected) {
      group.trackStates[rankedEntry.ftn] = TrackState::Selected;
      removeFromDeselectedQueue(group, rankedEntry.ftn);
      notifyTrackSelected(rankedEntry.ftn, group);
      return;
    }
  }
}

// Batch callback rationale: All sessions in a TopNGroup share the same N, so when a
// track enters their top-N, the relay can handle them together. Benefits:
// 1. Single upstream subscribe decision covers all sessions wanting this track
// 2. Relay can batch state updates (e.g., one forwarder lookup, one cache check)
// 3. Reduces per-session callback overhead when many sessions share the same N
// An alternative per-session callback exists (onSelected_) for cases needing individual handling.
void PropertyRanking::notifyTrackSelected(const moxygen::FullTrackName& ftn, TopNGroup& topNGroup) {
  std::vector<std::pair<std::shared_ptr<moxygen::MoQSession>, bool>> viewerBatch;
  for (auto& [session, info] : topNGroup.sessions) {
    if (isPublisher(session)) {
      reconcilePublisherSelection(info, topNGroup.maxSelected, session);
    } else {
      viewerBatch.emplace_back(session, info.forward);
    }
  }
  if (!viewerBatch.empty() && onBatchSelected_) {
    onBatchSelected_(ftn, viewerBatch);
  }
}

bool PropertyRanking::isSelfTrack(
    const moxygen::FullTrackName& ftn,
    const std::shared_ptr<moxygen::MoQSession>& session
) const {
  auto it = trackIndexByName_.find(ftn);
  if (it == trackIndexByName_.end()) {
    return false;
  }
  auto publisher = it->second.rankIter->second.publisher.lock();
  return publisher == session;
}

bool PropertyRanking::isPublisher(const std::shared_ptr<moxygen::MoQSession>& session) const {
  // O(1) lookup using cached publisher track count
  auto it = publisherTrackCount_.find(session.get());
  if (it == publisherTrackCount_.end()) {
    return false;
  }
  // Validate weak_ptr to guard against ABA problem (new session at same address)
  return it->second.session.lock() == session;
}

std::optional<RankKey> PropertyRanking::computeWaterlineKey(
    const std::shared_ptr<moxygen::MoQSession>& session,
    uint64_t maxSelected
) const {
  // Find the Nth non-self track. Its RankKey is the waterline: all non-self
  // tracks with key >= waterline should be selected for this session.
  // (RankKey uses std::greater<>, so higher key = lower rank = better.)
  uint64_t nonSelfCount = 0;
  for (const auto& [key, entry] : rankedTracks_) {
    // Inline self-track check: compare publisher directly (avoids redundant lookup)
    if (entry.publisher.lock() != session) {
      nonSelfCount++;
      if (nonSelfCount == maxSelected) {
        return key;
      }
    }
  }

  // Fewer than N non-self tracks exist — select all of them.
  return std::nullopt;
}

void PropertyRanking::reconcilePublisherSelection(
    SessionInfo& info,
    uint64_t maxSelected,
    const std::shared_ptr<moxygen::MoQSession>& session
) {
  // Recompute the publisher's personal waterline using dynamic self-exclusion.
  info.waterlineKey = computeWaterlineKey(session, maxSelected);

  // Determine what should currently be delivered to this publisher session.
  // A track is selected iff it's non-self AND at-or-above the waterline.
  folly::F14FastSet<moxygen::FullTrackName, moxygen::FullTrackName::hash> nowSelected;
  for (const auto& [key, entry] : rankedTracks_) {
    // Inline self-track check: compare publisher directly (avoids redundant lookup)
    if (entry.publisher.lock() != session && (!info.waterlineKey || key >= *info.waterlineKey)) {
      nowSelected.insert(entry.ftn);
    }
  }

  // Evict tracks that left the publisher's personal top-N.
  if (onEvicted_) {
    for (const auto& ftn : info.selectedTracks) {
      if (nowSelected.count(ftn) == 0) {
        onEvicted_(ftn, session);
      }
    }
  }

  // Notify tracks that entered the publisher's personal top-N.
  if (onSelected_) {
    for (const auto& ftn : nowSelected) {
      if (info.selectedTracks.count(ftn) == 0) {
        onSelected_(ftn, session, info.forward);
      }
    }
  }

  info.selectedTracks = std::move(nowSelected);
}

void PropertyRanking::reconcilePublisherInAllGroups(
    const std::shared_ptr<moxygen::MoQSession>& session
) {
  // When a track is registered by a session that is already subscribed,
  // that track becomes a self-track for this session. Reconcile all groups
  // where this session appears to update their selection accordingly.
  for (auto& [n, topNGroup] : topNGroups_) {
    auto sessionIt = topNGroup.sessions.find(session);
    if (sessionIt == topNGroup.sessions.end()) {
      continue;
    }

    auto& info = sessionIt->second;
    bool wasPublisher = !info.selectedTracks.empty() || info.waterlineKey.has_value();

    // If upgrading from viewer to publisher-subscriber, seed selectedTracks from
    // the shared top-N so reconcile can compute the correct delta (i.e. evict the
    // newly-self track rather than re-notifying every viewer-delivered track).
    if (!wasPublisher) {
      uint64_t rank = 0;
      for (const auto& [key, entry] : rankedTracks_) {
        if (rank >= n) {
          break;
        }
        info.selectedTracks.insert(entry.ftn);
        rank++;
      }
    }

    reconcilePublisherSelection(info, n, session);
  }
}

void PropertyRanking::sweepIdle() {
  if (idleTimeout_.count() == 0 || !getLastActivity_) {
    return;
  }

  auto now = std::chrono::steady_clock::now();

  for (auto& [n, topNGroup] : topNGroups_) {
    // Snapshot selected FTNs: deselecting mutates trackStates
    std::vector<moxygen::FullTrackName> selected;
    for (const auto& [ftn, state] : topNGroup.trackStates) {
      if (state == TrackState::Selected) {
        selected.push_back(ftn);
      }
    }

    for (const auto& ftn : selected) {
      auto lastActivity = getLastActivity_(ftn);
      // Epoch (default time_point) means the track never published an object.
      // Treat this as infinitely idle — a track that never sends data should
      // be evicted to make room for active tracks.
      bool neverPublished = (lastActivity == std::chrono::steady_clock::time_point{});
      if (!neverPublished && now - lastActivity <= idleTimeout_) {
        continue;
      }

      XLOG(DBG4) << "[PropertyRanking] Idle eviction: " << ftn << " (group N=" << n << ")"
                 << (neverPublished ? " [never published]" : "");

      // Demote into deselected queue and promote a replacement
      demoteTrackInGroup(topNGroup, ftn, 0 /* rank unknown */);
      promoteNextAvailableTrack(topNGroup, ftn);
    }
  }
}

} // namespace openmoq::moqx
