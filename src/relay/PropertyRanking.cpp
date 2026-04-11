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

void PropertyRanking::registerTrack(
    const moxygen::FullTrackName& ftn,
    std::optional<uint64_t> initialValue,
    std::weak_ptr<moxygen::MoQSession> publisher
) {
  if (trackIndexByName_.find(ftn) != trackIndexByName_.end()) {
    XLOG(WARN) << "Track already registered: " << ftn;
    return;
  }

  uint64_t value = initialValue.value_or(0);
  RankKey key{value, nextSeq_++};

  auto [iter, inserted] =
      rankedTracks_.emplace(key, RankedEntry{.ftn = ftn, .publisher = std::move(publisher)});
  trackIndexByName_[ftn] = RankIndex{.rankIter = iter, .cachedRank = UINT64_MAX};
  invalidateRankCache();

  uint64_t rank = getCachedRank(ftn);
  XLOG(DBG4) << "Registered track " << ftn << " with value " << value << " at rank " << rank;

  for (auto& [n, topNGroup] : topNGroups_) {
    if (rank < n) {
      topNGroup.trackStates[ftn] = TrackState::Selected;
      IterationGuard guard(*this);
      notifyTrackSelected(ftn, topNGroup);

      // The new track pushed the previous occupant of rank N-1 to rank N.
      // If that track was Selected, demote it into the deselected queue.
      demoteTrackAtRank(n, topNGroup);
    }
    // Tracks outside top N are not added to the deselected queue on register;
    // they only enter the queue when they fall out of an already-selected position.
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

  // Capture old rank before modifying the map (oldKey disappears after erase)
  uint64_t oldRank = getCachedRank(ftn);

  // Update the sorted map
  auto rankedEntry = std::move(entry.rankIter->second);
  rankedTracks_.erase(entry.rankIter);
  auto [newIter, _] = rankedTracks_.emplace(newKey, std::move(rankedEntry));
  entry.rankIter = newIter;
  invalidateRankCache();

  // Compute new rank now that the map reflects the new position
  uint64_t newRank = getCachedRank(ftn);

  // FAST PATH: value change doesn't cross any threshold
  if (!crossesThreshold(oldRank, newRank)) {
    return;
  }

  // Opportunistic idle sweep: a value change is worth a quick idle check
  sweepIdle();

  // SLOW PATH: value crossed a threshold, recompute TopNGroups
  recomputeTopNGroups(ftn, oldRank, newRank);
}

void PropertyRanking::removeTrack(const moxygen::FullTrackName& ftn) {
  auto it = trackIndexByName_.find(ftn);
  if (it == trackIndexByName_.end()) {
    return;
  }

  auto& entry = it->second;
  uint64_t oldRank = getCachedRank(ftn);

  rankedTracks_.erase(entry.rankIter);
  trackIndexByName_.erase(it);
  invalidateRankCache();

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
        topNGroup.trackStates[promoted] = TrackState::Selected;
        notifyTrackSelected(promoted, topNGroup);
      } else {
        // Fall back to ranked list scan: find the highest-ranked non-selected track
        uint64_t count = 0;
        for (auto& [key, rankedEntry] : rankedTracks_) {
          auto stIt = topNGroup.trackStates.find(rankedEntry.ftn);
          if (stIt != topNGroup.trackStates.end() && stIt->second == TrackState::Selected) {
            count++;
            continue;
          }
          // count selected tracks above this one; if < n, this track belongs in top-N
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

void PropertyRanking::removeSessionFromTopNGroup(
    uint64_t maxSelected,
    const std::shared_ptr<moxygen::MoQSession>& session
) {
  XCHECK(!iteratingSessions_) << "Cannot remove session while iterating";

  auto it = topNGroups_.find(maxSelected);
  if (it == topNGroups_.end()) {
    return;
  }

  it->second.sessions.erase(session);

  if (it->second.sessions.empty()) {
    removeTopNGroup(maxSelected);
  }
}

void PropertyRanking::removeTopNGroup(uint64_t maxSelected) {
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

uint64_t PropertyRanking::getCachedRank(const moxygen::FullTrackName& ftn) const {
  rebuildRankCacheIfNeeded();
  auto it = trackIndexByName_.find(ftn);
  if (it == trackIndexByName_.end()) {
    return UINT64_MAX;
  }
  return it->second.cachedRank;
}

bool PropertyRanking::crossesThreshold(uint64_t oldRank, uint64_t newRank) const {
  if (topNGroups_.empty()) {
    return false;
  }

  if (oldRank >= selectionThreshold_ && newRank >= selectionThreshold_) {
    return false;
  }

  uint64_t minRank = std::min(oldRank, newRank);
  uint64_t maxRank = std::max(oldRank, newRank);

  bool oldInPool = oldRank < selectionThreshold_;
  bool newInPool = newRank < selectionThreshold_;
  if (oldInPool != newInPool) {
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

    // Batch-notify all sessions when track enters shared top-N
    if (!wasInTopN && nowInTopN) {
      IterationGuard guard(*this);
      notifyTrackSelected(ftn, topNGroup);

      // The track that just entered top-N displaced the one at position n;
      // mark that track as deselected.
      demoteTrackAtRank(n, topNGroup);
    }

    // When the track fell out of top-N, promote the one now at position n-1.
    if (wasInTopN && !nowInTopN) {
      IterationGuard guard(*this);
      uint64_t count = 0;
      for (auto& [key, rankedEntry] : rankedTracks_) {
        if (count == n - 1) {
          auto stIt = topNGroup.trackStates.find(rankedEntry.ftn);
          if (stIt == topNGroup.trackStates.end() || stIt->second != TrackState::Selected) {
            topNGroup.trackStates[rankedEntry.ftn] = TrackState::Selected;
            removeFromDeselectedQueue(topNGroup, rankedEntry.ftn);
            notifyTrackSelected(rankedEntry.ftn, topNGroup);
          }
          break;
        }
        count++;
      }
    }
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

void PropertyRanking::removeFromDeselectedQueue(
    TopNGroup& group,
    const moxygen::FullTrackName& ftn
) {
  auto& dq = group.deselectedQueue;
  dq.erase(std::remove(dq.begin(), dq.end(), ftn), dq.end());
}

void PropertyRanking::demoteTrackAtRank(uint64_t n, TopNGroup& group) {
  uint64_t count = 0;
  for (auto& [key, rankedEntry] : rankedTracks_) {
    if (count == n) {
      auto stIt = group.trackStates.find(rankedEntry.ftn);
      if (stIt != group.trackStates.end() && stIt->second == TrackState::Selected) {
        stIt->second = TrackState::Deselected;
        group.deselectedQueue.push_back(rankedEntry.ftn);
        trimDeselectedQueue(group);
      }
      break;
    }
    count++;
  }
}

void PropertyRanking::notifyTrackSelected(const moxygen::FullTrackName& ftn, TopNGroup& topNGroup) {
  std::vector<std::pair<std::shared_ptr<moxygen::MoQSession>, bool>> batch;
  for (const auto& [session, info] : topNGroup.sessions) {
    batch.emplace_back(session, info.forward);
  }
  if (!batch.empty() && onBatchSelected_) {
    onBatchSelected_(ftn, batch);
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
      // Skip tracks with no recorded activity (epoch = never seen an object).
      // These are brand-new tracks or tracks whose activityTarget is not wired,
      // and must not be treated as idle.
      if (lastActivity == std::chrono::steady_clock::time_point{}) {
        continue;
      }
      if (now - lastActivity <= idleTimeout_) {
        continue;
      }

      XLOG(DBG4) << "[PropertyRanking] Idle eviction: " << ftn << " (group N=" << n << ")";

      // Demote into deselected queue — same path as ranking-based eviction.
      // Allows cheap reselection if the track resumes publishing.
      topNGroup.trackStates[ftn] = TrackState::Deselected;
      topNGroup.deselectedQueue.push_back(ftn);
      trimDeselectedQueue(topNGroup);

      // Promote the highest-ranked non-selected track into the opened slot.
      // Scan rankedTracks_ from the top; skip Selected and the idle track itself.
      IterationGuard guard(*this);
      for (const auto& [key, rankedEntry] : rankedTracks_) {
        if (rankedEntry.ftn == ftn) {
          continue; // skip the track we just marked idle
        }
        auto stIt = topNGroup.trackStates.find(rankedEntry.ftn);
        bool isSelected =
            stIt != topNGroup.trackStates.end() && stIt->second == TrackState::Selected;
        if (!isSelected) {
          topNGroup.trackStates[rankedEntry.ftn] = TrackState::Selected;
          removeFromDeselectedQueue(topNGroup, rankedEntry.ftn);
          notifyTrackSelected(rankedEntry.ftn, topNGroup);
          break;
        }
      }
    }
  }
}

} // namespace openmoq::moqx
