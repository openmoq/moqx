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
    BatchSelectCallback onBatchSelected,
    SelectCallback onSelected,
    EvictCallback onEvicted
)
    : propertyType_(propertyType), maxDeselected_(maxDeselected),
      onBatchSelected_(std::move(onBatchSelected)), onSelected_(std::move(onSelected)),
      onEvicted_(std::move(onEvicted)) {}

void PropertyRanking::registerTrack(
    const moxygen::FullTrackName& ftn,
    std::optional<uint64_t> initialValue,
    std::weak_ptr<moxygen::MoQSession> publisher
) {
  if (trackIndexByName_.find(ftn) != trackIndexByName_.end()) {
    XLOG(WARN) << "Track already registered: " << ftn;
    return;
  }

  // Ensure cache is valid before insertion so we can use iterator-- for O(1) rank lookup
  rebuildRankCacheIfNeeded();

  uint64_t value = initialValue.value_or(0);
  RankKey key{value, nextSeq_++};

  auto [iter, inserted] =
      rankedTracks_.emplace(key, RankedEntry{.ftn = ftn, .publisher = std::move(publisher)});

  // Compute rank in O(1) using previous track's cached rank
  uint64_t rank;
  if (iter == rankedTracks_.begin()) {
    rank = 0;
  } else {
    auto prevIt = iter;
    --prevIt;
    auto prevIndexIt = trackIndexByName_.find(prevIt->second.ftn);
    rank = (prevIndexIt != trackIndexByName_.end()) ? prevIndexIt->second.cachedRank + 1 : 0;
  }

  trackIndexByName_[ftn] = RankIndex{.rankIter = iter, .cachedRank = rank};

  // Increment cached ranks for all tracks after insertion point: O(numTracks - rank)
  auto nextIt = iter;
  ++nextIt;
  for (; nextIt != rankedTracks_.end(); ++nextIt) {
    auto indexIt = trackIndexByName_.find(nextIt->second.ftn);
    if (indexIt != trackIndexByName_.end()) {
      indexIt->second.cachedRank++;
    }
  }
  // Cache remains valid - no full rebuild needed

  XLOG(DBG4) << "Registered track " << ftn << " with value " << value << " at rank " << rank;

  // Two-phase algorithm: O(G log G + N) instead of O(G * N)
  // Phase 1: Mark track selected in applicable groups, collect demotion positions
  std::vector<std::pair<uint64_t, TopNGroup*>> demotions;  // (position n, group)

  for (auto& [n, topNGroup] : topNGroups_) {
    if (rank < n) {
      topNGroup.trackStates[ftn] = TrackState::Selected;
      notifyTrackSelected(ftn, topNGroup);

      // The new track pushed the previous occupant of rank N-1 to rank N.
      // Queue demotion (only fires when numTracks > N).
      demotions.emplace_back(n, &topNGroup);
    }
    // Tracks outside top N are not added to the deselected queue on register;
    // they only enter the queue when they fall out of an already-selected position.
  }

  // Phase 2: Single traversal to demote tracks at collected positions
  if (!demotions.empty()) {
    IterationGuard guard(*this);

    std::sort(demotions.begin(), demotions.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    size_t demIdx = 0;
    uint64_t count = 0;
    for (auto& [key, rankedEntry] : rankedTracks_) {
      while (demIdx < demotions.size() && count == demotions[demIdx].first) {
        demoteTrackInGroup(*demotions[demIdx].second, rankedEntry.ftn, count);
        demIdx++;
      }
      if (demIdx >= demotions.size()) {
        break;
      }
      count++;
    }
  }
}

void PropertyRanking::updateSortValue(const moxygen::FullTrackName& ftn, uint64_t value) {
  auto it = trackIndexByName_.find(ftn);
  if (it == trackIndexByName_.end()) {
    XLOG(DBG4) << "updateSortValue: track not registered " << ftn;
    return;
  }

  auto& entry = it->second;
  RankKey oldKey = entry.rankIter->first;

  // Early exit: no change in value
  if (oldKey.value == value) {
    return;
  }

  // Construct new key after the early-exit check
  RankKey newKey{value, oldKey.arrivalSeq};

  // Ensure cache is valid and capture old rank before modifying the map
  rebuildRankCacheIfNeeded();
  uint64_t oldRank = entry.cachedRank;

  // Update the sorted map: O(log N) erase + insert
  auto rankedEntry = std::move(entry.rankIter->second);
  rankedTracks_.erase(entry.rankIter);
  auto [newIter, _] = rankedTracks_.emplace(newKey, std::move(rankedEntry));
  entry.rankIter = newIter;

  // Compute new rank using std::distance: O(newRank) for bidirectional iterator.
  // For top-ranked tracks (where we care most), this is fast.
  uint64_t newRank = static_cast<uint64_t>(
      std::distance(rankedTracks_.begin(), newIter));
  entry.cachedRank = newRank;

  // Incrementally update cached ranks for affected tracks only: O(|oldRank - newRank|).
  // Tracks between old and new positions had their ranks shift by ±1.
  if (newRank < oldRank) {
    // Track moved up (higher value → lower rank). Tracks in (newRank, oldRank] shift down.
    auto rankedIt = newIter;
    ++rankedIt;  // Skip the moved track
    for (uint64_t r = newRank + 1; r <= oldRank && rankedIt != rankedTracks_.end();
         ++r, ++rankedIt) {
      auto indexIt = trackIndexByName_.find(rankedIt->second.ftn);
      if (indexIt != trackIndexByName_.end()) {
        indexIt->second.cachedRank = r;
      }
    }
  } else if (newRank > oldRank) {
    // Track moved down (lower value → higher rank). Tracks in [oldRank, newRank) shift up.
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

  // FAST PATH: value change doesn't cross any threshold
  if (!crossesThreshold(oldRank, newRank)) {
    XLOG(DBG5) << "updateSortValue fast path: " << ftn << " value=" << value
               << " rank " << oldRank << " -> " << newRank << " (no threshold crossed)";
    return;
  }

  // SLOW PATH: value crossed a threshold, recompute TopNGroups
  XLOG(DBG4) << "updateSortValue slow path: " << ftn << " value=" << value
             << " rank " << oldRank << " -> " << newRank << " (threshold crossed)";
  recomputeTopNGroups(ftn, oldRank, newRank);
}

void PropertyRanking::removeTrack(const moxygen::FullTrackName& ftn) {
  auto it = trackIndexByName_.find(ftn);
  if (it == trackIndexByName_.end()) {
    return;
  }

  // Ensure cache is valid before removal
  rebuildRankCacheIfNeeded();

  auto& entry = it->second;
  uint64_t oldRank = entry.cachedRank;

  // Save iterator to next track before erasing (remains valid after erase per std::map guarantees)
  auto nextIt = entry.rankIter;
  ++nextIt;

  rankedTracks_.erase(entry.rankIter);
  trackIndexByName_.erase(it);

  // Decrement cached ranks for all tracks after removal point: O(numTracks - oldRank)
  for (; nextIt != rankedTracks_.end(); ++nextIt) {
    auto indexIt = trackIndexByName_.find(nextIt->second.ftn);
    if (indexIt != trackIndexByName_.end()) {
      indexIt->second.cachedRank--;
    }
  }
  // Cache remains valid - no full rebuild needed

  XLOG(DBG4) << "Removed track " << ftn << " from rank " << oldRank;

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
      IterationGuard guard(*this);

      // Try deselected queue first for cheap promotion
      if (!topNGroup.deselectedQueue.empty()) {
        auto promoted = topNGroup.deselectedQueue.front();
        topNGroup.deselectedQueue.pop_front();
        XLOG(DBG4) << "removeTrack: promoting " << promoted
                   << " from deselected queue for TopNGroup maxSelected=" << n;
        topNGroup.trackStates[promoted] = TrackState::Selected;
        notifyTrackSelected(promoted, topNGroup);
      } else {
        // Fallback: deselected queue is empty (e.g., first removal after registration
        // when no tracks have yet been demoted). Scan ranked list for the first
        // non-selected track to promote into the vacated slot.
        for (auto& [key, rankedEntry] : rankedTracks_) {
          auto stIt = topNGroup.trackStates.find(rankedEntry.ftn);
          if (stIt != topNGroup.trackStates.end() && stIt->second == TrackState::Selected) {
            continue;
          }
          // Deselected queue is empty — scan ranked list for the first non-selected
          // track. Since we just removed one selected track, there's guaranteed room
          // in top-N for exactly one promotion.
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
    updateSelectionThreshold();

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
  XLOG(DBG4) << "addSessionToTopNGroup: maxSelected=" << maxSelected
             << " forward=" << forward;

  auto& topNGroup = getOrCreateTopNGroup(maxSelected);
  topNGroup.sessions[session] = SessionInfo{.forward = forward};

  // Notify session of all currently-selected tracks
  uint64_t rank = 0;
  for (auto& [key, entry] : rankedTracks_) {
    if (rank >= maxSelected) {
      break;
    }
    if (onSelected_) {
      onSelected_(entry.ftn, session, forward);
    }
    rank++;
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
    XLOG(WARN) << "updateSessionForward: session not found in TopNGroup maxSelected=" << maxSelected;
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

// O(numTracks) full cache rebuild. Only called when cache is invalid (e.g., after
// direct map manipulation in tests). Normal operations use incremental updates:
// - registerTrack: O(1) rank lookup via iterator--, O(numTracks - rank) updates
// - removeTrack: O(numTracks - oldRank) updates
// - updateSortValue: O(newRank) + O(|movement|) updates
//
// Future optimization opportunity:
// Lazy partial cache: don't compute ranks for tracks past max(N) + maxDeselected.
// Their rank only matters on track removal or increasing N (not yet supported).
void PropertyRanking::rebuildRankCacheIfNeeded() const {
  if (rankCacheValid_) {
    return;
  }
  uint64_t rank = 0;
  for (const auto& [key, entry] : rankedTracks_) {
    auto it = trackIndexByName_.find(entry.ftn);
    if (it != trackIndexByName_.end()) {
      const_cast<RankIndex&>(it->second).cachedRank = rank;
    }
    rank++;
  }
  rankCacheValid_ = true;
}

bool PropertyRanking::crossesThreshold(uint64_t oldRank, uint64_t newRank) const {
  if (topNGroups_.empty()) {
    return false;
  }

  // Fast exit: both ranks outside selection region - no threshold can be crossed.
  if (oldRank >= selectionThreshold_ && newRank >= selectionThreshold_) {
    return false;
  }
  // NOTE: We intentionally do NOT fast-exit when both ranks are below selectionThreshold_.
  // Even though both are within the selection region, a specific N boundary might still
  // be crossed (e.g., moving from rank 5 to rank 2 crosses N=3 but not N=7).

  uint64_t minRank = std::min(oldRank, newRank);
  uint64_t maxRank = std::max(oldRank, newRank);

  // Check if track crossed into/out of the selection region entirely.
  bool oldInAnyTopN = oldRank < selectionThreshold_;
  bool newInAnyTopN = newRank < selectionThreshold_;
  if (oldInAnyTopN != newInAnyTopN) {
    return true;
  }

  // O(log G) check: any threshold between minRank and maxRank?
  auto it = std::upper_bound(sortedThresholds_.begin(), sortedThresholds_.end(), minRank);
  if (it != sortedThresholds_.end() && *it <= maxRank) {
    return true;
  }

  return false;
}

void PropertyRanking::recomputeTopNGroups(
    const moxygen::FullTrackName& ftn,
    uint64_t oldRank,
    uint64_t newRank
) {
  XLOG(DBG4) << "recomputeTopNGroups: " << ftn << " moved from rank " << oldRank << " to "
             << newRank;

  IterationGuard guard(*this);

  // Two-phase algorithm: O(G log G + N) instead of O(G * N)
  // Phase 1: Update state and collect boundary positions needing demotion/promotion
  enum class Action { Demote, Promote };
  std::vector<std::tuple<uint64_t, TopNGroup*, Action>> boundaryOps;

  for (auto& [n, topNGroup] : topNGroups_) {
    bool wasInTopN = oldRank < n;
    bool nowInTopN = newRank < n;

    if (wasInTopN != nowInTopN) {
      if (nowInTopN) {
        topNGroup.trackStates[ftn] = TrackState::Selected;
        removeFromDeselectedQueue(topNGroup, ftn);
      } else {
        topNGroup.trackStates[ftn] = TrackState::Deselected;
        topNGroup.deselectedQueue.push_back(ftn);
        trimDeselectedQueue(topNGroup);
      }
    }

    // Track entered top-N: notify and queue demotion at position n
    if (!wasInTopN && nowInTopN) {
      notifyTrackSelected(ftn, topNGroup);
      boundaryOps.emplace_back(n, &topNGroup, Action::Demote);
    }

    // Track fell out of top-N: queue promotion at position n-1
    if (wasInTopN && !nowInTopN) {
      boundaryOps.emplace_back(n - 1, &topNGroup, Action::Promote);
    }
  }

  // Phase 2: Single traversal of rankedTracks_ to handle all boundary operations
  if (boundaryOps.empty()) {
    return;
  }

  // Sort by position so we can process in one pass
  std::sort(boundaryOps.begin(), boundaryOps.end(),
            [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });

  size_t opIdx = 0;
  uint64_t count = 0;
  for (auto& [key, rankedEntry] : rankedTracks_) {
    // Process all operations at this position
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
      break;  // All operations processed
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
  while (topNGroup.deselectedQueue.size() > maxDeselected_) {
    auto evicted = topNGroup.deselectedQueue.front();
    topNGroup.deselectedQueue.pop_front();
    topNGroup.trackStates.erase(evicted);

    XLOG(DBG4) << "[PropertyRanking] Evicting track " << evicted
               << " from TopNGroup maxSelected=" << topNGroup.maxSelected
               << " (deselectedQueue exceeded maxDeselected=" << maxDeselected_ << ")";

    IterationGuard guard(*this);
    for (const auto& [session, info] : topNGroup.sessions) {
      if (session && onEvicted_) {
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
    XLOG(DBG4) << "demoteTrackInGroup: demoting " << ftn
               << " at rank " << rank << " to deselected queue";
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
    XLOG(DBG4) << "promoteTrackInGroup: promoting " << ftn
               << " at rank " << rank << " into top-N";
    group.trackStates[ftn] = TrackState::Selected;
    removeFromDeselectedQueue(group, ftn);
    notifyTrackSelected(ftn, group);
  }
}

// Batch callback rationale: All sessions in a TopNGroup share the same N, so when a
// track enters their top-N, the relay can handle them together. Benefits:
// 1. Single upstream subscribe decision covers all sessions wanting this track
// 2. Relay can batch state updates (e.g., one forwarder lookup, one cache check)
// 3. Reduces per-session callback overhead when many sessions share the same N
// An alternative per-session callback exists (onSelected_) for cases needing individual handling.
void PropertyRanking::notifyTrackSelected(const moxygen::FullTrackName& ftn, TopNGroup& topNGroup) {
  std::vector<std::pair<std::shared_ptr<moxygen::MoQSession>, bool>> batch;
  for (const auto& [session, info] : topNGroup.sessions) {
    batch.emplace_back(session, info.forward);
  }
  if (!batch.empty() && onBatchSelected_) {
    XLOG(DBG4) << "notifyTrackSelected: " << ftn << " to " << batch.size()
               << " sessions in TopNGroup maxSelected=" << topNGroup.maxSelected;
    onBatchSelected_(ftn, batch);
  }
}

} // namespace openmoq::moqx
