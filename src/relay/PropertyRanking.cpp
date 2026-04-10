/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Originally from github.com/facebookexperimental/moxygen.
 * See deps/moxygen/LICENSE for the original license terms.
 *
 * Copyright (c) OpenMOQ contributors.
 */

#include "relay/PropertyRanking.h"

#include <folly/logging/xlog.h>

namespace openmoq::moqx {

PropertyRanking::PropertyRanking(
    uint64_t propertyType,
    uint64_t maxDeselected,
    BatchSelectCallback onBatchSelected,
    SelectCallback onSelected,
    EvictCallback onEvicted)
    : propertyType_(propertyType),
      maxDeselected_(maxDeselected),
      onBatchSelected_(std::move(onBatchSelected)),
      onSelected_(std::move(onSelected)),
      onEvicted_(std::move(onEvicted)) {}

void PropertyRanking::registerTrack(
    const moxygen::FullTrackName& ftn,
    std::optional<uint64_t> initialValue,
    std::weak_ptr<moxygen::MoQSession> publisher) {
  if (stats_.enabled) {
    stats_.tracksRegistered++;
  }

  // Check if already registered
  if (tracks_.find(ftn) != tracks_.end()) {
    XLOG(WARN) << "Track already registered: " << ftn;
    return;
  }

  // Use initial value or 0 as default
  uint64_t value = initialValue.value_or(0);
  RankKey key{value, nextSeq_++};

  // Insert into sorted map
  auto [iter, inserted] = rankedTracks_.emplace(
      key, RankedEntry{.ftn = ftn, .publisher = std::move(publisher)});

  // Store iterator for O(1) lookup
  tracks_[ftn] = RankIndex{.rankIter = iter, .cachedRank = UINT64_MAX};

  // Invalidate rank cache since structure changed
  invalidateRankCache();

  XLOG(DBG4) << "Registered track " << ftn << " with value " << value
             << " at rank " << getRank(key);

  // Check if this track should be selected for any TopNGroup
  uint64_t rank = getRank(key);
  for (auto& [n, topNGroup] : topNGroups_) {
    if (rank < n) {
      // Track is in top N, mark as selected
      topNGroup.trackStates[ftn] = TrackState::Selected;

      // Use iteration guard and notify sessions
      IterationGuard guard(*this);
      notifyTrackSelected(ftn, topNGroup);
    }
    // Note: Tracks outside top N are NOT added to deselected queue on register.
    // They only enter the queue when they fall out of top N after being selected.
  }
}

void PropertyRanking::updateSortValue(const moxygen::FullTrackName& ftn, uint64_t value) {
  auto it = tracks_.find(ftn);
  if (it == tracks_.end()) {
    // Track not registered, ignore
    XLOG(DBG4) << "updateSortValue: track not registered " << ftn;
    return;
  }

  auto& entry = it->second;
  RankKey oldKey = entry.rankIter->first;
  RankKey newKey{value, oldKey.arrivalSeq};

  // No change in value
  if (oldKey.value == newKey.value) {
    return;
  }

  if (stats_.enabled) {
    stats_.valueUpdates++;
  }

  // FAST PATH: value change doesn't cross any threshold
  if (!crossesThreshold(oldKey, newKey)) {
    if (stats_.enabled) {
      stats_.fastPathHits++;
    }
    // Just update position in sorted map using extract/insert
    auto rankedEntry = std::move(entry.rankIter->second);
    rankedTracks_.erase(entry.rankIter);
    auto [newIter, _] = rankedTracks_.emplace(newKey, std::move(rankedEntry));
    entry.rankIter = newIter;
    // Invalidate cache - positions shifted
    invalidateRankCache();
    return;
  }

  if (stats_.enabled) {
    stats_.slowPathHits++;
  }

  // SLOW PATH: value crossed a threshold, recompute TopNGroups
  auto rankedEntry = std::move(entry.rankIter->second);
  rankedTracks_.erase(entry.rankIter);
  auto [newIter, _] = rankedTracks_.emplace(newKey, std::move(rankedEntry));
  entry.rankIter = newIter;

  // Invalidate cache - positions shifted
  invalidateRankCache();

  recomputeTopNGroups(ftn, oldKey, newKey);
}

void PropertyRanking::removeTrack(const moxygen::FullTrackName& ftn) {
  auto it = tracks_.find(ftn);
  if (it == tracks_.end()) {
    return;
  }

  if (stats_.enabled) {
    stats_.tracksRemoved++;
  }

  auto& entry = it->second;
  RankKey oldKey = entry.rankIter->first;
  uint64_t oldRank = getRank(oldKey);

  // Remove from ranked map
  rankedTracks_.erase(entry.rankIter);
  tracks_.erase(it);

  // Invalidate rank cache since structure changed
  invalidateRankCache();

  XLOG(DBG4) << "Removed track " << ftn << " from rank " << oldRank;

  // Update TopNGroups - removed track may cause promotions
  for (auto& [n, topNGroup] : topNGroups_) {
    auto stateIt = topNGroup.trackStates.find(ftn);
    if (stateIt == topNGroup.trackStates.end()) {
      continue;
    }

    bool wasSelected = stateIt->second == TrackState::Selected;
    topNGroup.trackStates.erase(stateIt);

    // Remove from deselected queue if present
    auto& dq = topNGroup.deselectedQueue;
    dq.erase(std::remove(dq.begin(), dq.end(), ftn), dq.end());

    if (wasSelected) {
      // Use iteration guard - callbacks must not add/remove sessions
      IterationGuard guard(*this);

      // Need to promote next track from deselected queue or ranked list
      // First try deselected queue
      if (!topNGroup.deselectedQueue.empty()) {
        auto promoted = topNGroup.deselectedQueue.front();
        topNGroup.deselectedQueue.pop_front();
        topNGroup.trackStates[promoted] = TrackState::Selected;
        notifyTrackSelected(promoted, topNGroup);
      } else {
        // Find next track from ranked list that isn't already selected
        // TODO: This nested loop could be optimized by processing TopNGroups
        // in descending order of N, or by maintaining a "first unselected" pointer.
        uint64_t count = 0;
        for (auto& [key, rankedEntry] : rankedTracks_) {
          auto stIt = topNGroup.trackStates.find(rankedEntry.ftn);
          if (stIt != topNGroup.trackStates.end() &&
              stIt->second == TrackState::Selected) {
            count++;
            continue;
          }
          // This track is not selected, promote it if we have room
          if (count < n) {
            topNGroup.trackStates[rankedEntry.ftn] = TrackState::Selected;
            notifyTrackSelected(rankedEntry.ftn, topNGroup);
            break;
          }
          count++;
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

    // Initialize track states for existing tracks
    uint64_t rank = 0;
    for (auto& [key, entry] : rankedTracks_) {
      if (rank < maxSelected) {
        it->second.trackStates[entry.ftn] = TrackState::Selected;
      }
      // Note: Tracks outside top N are NOT added to deselected queue.
      // They only enter the queue when they fall out of top N.
      rank++;
    }
  }
  return it->second;
}

void PropertyRanking::addSessionToTopNGroup(
    uint64_t maxSelected,
    std::shared_ptr<moxygen::MoQSession> session,
    bool forward,
    std::vector<moxygen::FullTrackName> publishedTracks) {
  // Ensure we're not modifying sessions while iterating
  XCHECK(!iteratingSessions_) << "Cannot add session while iterating";

  XLOG(DBG4) << "[PropertyRanking] Adding session " << session.get()
             << " to TopNGroup maxSelected=" << maxSelected
             << " forward=" << forward
             << " publishedTracks=" << publishedTracks.size();

  if (stats_.enabled) {
    stats_.sessionsAdded++;
  }

  auto& topNGroup = getOrCreateTopNGroup(maxSelected);
  SessionInfo info{.forward = forward};

  // Set up self-exclusion if this is a publisher-subscriber
  for (auto& ftn : publishedTracks) {
    XLOG(DBG4) << "[PropertyRanking] Session " << session.get()
               << " self-exclusion: will skip own track " << ftn;
    info.publishedTracks.insert(std::move(ftn));
  }

  // Compute waterline if publisher
  if (info.isPublisher()) {
    info.waterlineKey = computeWaterlineKey(info, maxSelected);
    info.waterlineValid = true;
    if (info.waterlineKey) {
      XLOG(DBG4) << "[PropertyRanking] Session " << session.get()
                 << " waterline computed: value=" << info.waterlineKey->value
                 << " arrivalSeq=" << info.waterlineKey->arrivalSeq;
    } else {
      XLOG(DBG4) << "[PropertyRanking] Session " << session.get()
                 << " waterline: none (fewer than " << maxSelected
                 << " non-self tracks)";
    }
  }

  topNGroup.sessions[session] = std::move(info);

  // Notify session of tracks it should receive (respecting self-exclusion)
  const auto& sessionInfo = topNGroup.sessions[session];
  for (auto& [key, entry] : rankedTracks_) {
    if (shouldSelectForSession(entry.ftn, key, sessionInfo, maxSelected)) {
      XLOG(DBG4) << "[PropertyRanking] Selecting track " << entry.ftn
                 << " for session " << session.get()
                 << " (value=" << key.value << ")";
      if (onSelected_) {
        onSelected_(entry.ftn, session, sessionInfo.forward);
      }
    }
  }
}

void PropertyRanking::removeSessionFromTopNGroup(
    uint64_t maxSelected,
    const std::shared_ptr<moxygen::MoQSession>& session) {
  // Ensure we're not modifying sessions while iterating
  XCHECK(!iteratingSessions_) << "Cannot remove session while iterating";

  auto it = topNGroups_.find(maxSelected);
  if (it == topNGroups_.end()) {
    return;
  }

  if (stats_.enabled) {
    stats_.sessionsRemoved++;
  }

  it->second.sessions.erase(session);

  // Remove TopNGroup if no sessions remain
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
  // Rebuild cache if invalidated, then return O(1) lookup
  rebuildRankCacheIfNeeded();
  auto trackIt = tracks_.find(it->second.ftn);
  if (trackIt != tracks_.end()) {
    return trackIt->second.cachedRank;
  }
  // Fallback (shouldn't happen if data structures are consistent)
  return std::distance(rankedTracks_.begin(), it);
}

void PropertyRanking::rebuildRankCacheIfNeeded() const {
  if (rankCacheValid_) {
    return;
  }
  uint64_t rank = 0;
  for (const auto& [key, entry] : rankedTracks_) {
    auto it = tracks_.find(entry.ftn);
    if (it != tracks_.end()) {
      const_cast<RankIndex&>(it->second).cachedRank = rank;
    }
    rank++;
  }
  rankCacheValid_ = true;
}

uint64_t PropertyRanking::getCachedRank(const moxygen::FullTrackName& ftn) const {
  rebuildRankCacheIfNeeded();
  auto it = tracks_.find(ftn);
  if (it == tracks_.end()) {
    return UINT64_MAX;
  }
  return it->second.cachedRank;
}

bool PropertyRanking::crossesThreshold(
    std::optional<RankKey> oldKey,
    const RankKey& newKey) const {
  if (topNGroups_.empty()) {
    return false;
  }

  // Get positions in sorted order
  uint64_t oldRank = oldKey ? getRank(*oldKey) : UINT64_MAX;

  // For newKey, we need to find where it would be inserted
  // Since we already updated rankedTracks_, we can use getRank directly
  uint64_t newRank = getRank(newKey);

  // OPTIMIZATION: Early exit if both ranks are far from any threshold
  // If both are beyond selection threshold, no threshold can be crossed
  if (oldRank >= selectionThreshold_ && newRank >= selectionThreshold_) {
    return false;
  }

  uint64_t minRank = std::min(oldRank, newRank);
  uint64_t maxRank = std::max(oldRank, newRank);

  // Check if crossed selection threshold
  bool oldInPool = oldRank < selectionThreshold_;
  bool newInPool = newRank < selectionThreshold_;
  if (oldInPool != newInPool) {
    return true;
  }

  // OPTIMIZATION: O(log G) check using sorted thresholds instead of O(G) linear scan.
  //
  // sortedThresholds_ contains all N values from TopNGroups in ascending order.
  // A threshold is "crossed" if the track moved from one side of it to the other.
  //
  // Example: thresholds = [1, 3, 5], track moved from rank 2 to rank 4
  //   minRank=2, maxRank=4
  //   upper_bound(2) returns iterator to 3 (first threshold > 2)
  //   3 <= 4, so threshold 3 was crossed (track went from top-3 to not-top-3)
  //
  // If no threshold exists in (minRank, maxRank], the track stayed in the same
  // selection bucket for all groups and we can skip recomputation (fast path).
  auto it = std::upper_bound(sortedThresholds_.begin(), sortedThresholds_.end(), minRank);
  if (it != sortedThresholds_.end() && *it <= maxRank) {
    return true;  // At least one threshold is crossed
  }

  return false;
}

void PropertyRanking::recomputeTopNGroups(
    const moxygen::FullTrackName& ftn,
    std::optional<RankKey> oldKey,
    const RankKey& newKey) {
  uint64_t oldRank = oldKey ? getRank(*oldKey) : UINT64_MAX;
  uint64_t newRank = getRank(newKey);

  XLOG(DBG4) << "recomputeTopNGroups: " << ftn << " moved from rank " << oldRank
             << " to " << newRank;

  for (auto& [n, topNGroup] : topNGroups_) {
    bool wasInTopN = oldRank < n;
    bool nowInTopN = newRank < n;

    // Update shared track state (for viewers)
    if (wasInTopN != nowInTopN) {
      if (nowInTopN) {
        topNGroup.trackStates[ftn] = TrackState::Selected;
        auto& dq = topNGroup.deselectedQueue;
        dq.erase(std::remove(dq.begin(), dq.end(), ftn), dq.end());
      } else {
        topNGroup.trackStates[ftn] = TrackState::Deselected;
        topNGroup.deselectedQueue.push_back(ftn);
        trimDeselectedQueue(topNGroup);
      }
    }

    // Notification logic differs for viewers vs publishers:
    // - Viewers: batch notification when track enters shared top-N
    // - Publishers: individual notification based on per-session waterline
    //
    // We collect notifications first, then send after iteration to avoid
    // issues if callbacks modify state. IterationGuard prevents add/remove.
    IterationGuard guard(*this);

    std::vector<std::pair<std::shared_ptr<moxygen::MoQSession>, bool>> viewerBatch;
    bool needViewerNotification = !wasInTopN && nowInTopN;

    std::vector<std::pair<std::shared_ptr<moxygen::MoQSession>, bool>> publisherNotifications;

    for (auto& [session, info] : topNGroup.sessions) {
      if (info.isPublisher()) {
        // Skip self tracks entirely (no notification needed)
        if (info.isSelfTrack(ftn)) {
          XLOG(DBG5) << "[PropertyRanking] Skipping self-track " << ftn
                     << " for session " << session.get();
          // OPTIMIZATION: Only recompute waterline if self-track crossed N boundary
          bool selfTrackCrossedN = (oldRank < n) != (newRank < n);
          if (!info.waterlineValid || selfTrackCrossedN) {
            auto oldWaterline = info.waterlineKey;
            info.waterlineKey = computeWaterlineKey(info, n);
            info.waterlineValid = true;
            if (oldWaterline != info.waterlineKey) {
              XLOG(DBG4) << "[PropertyRanking] Waterline changed for session "
                         << session.get() << " (self-track crossed N): "
                         << (oldWaterline ? std::to_string(oldWaterline->value) : "none")
                         << " -> "
                         << (info.waterlineKey ? std::to_string(info.waterlineKey->value) : "none");
            }
          }
          continue;
        }

        // Non-self track moved - waterline stays valid unless already invalidated
        if (!info.waterlineValid) {
          info.waterlineKey = computeWaterlineKey(info, n);
          info.waterlineValid = true;
        }

        // Check if this track's selection status changed for this session
        bool wasSelected =
            oldKey ? shouldSelectForSession(ftn, *oldKey, info, n) : false;
        bool nowSelected = shouldSelectForSession(ftn, newKey, info, n);

        if (!wasSelected && nowSelected) {
          XLOG(DBG4) << "[PropertyRanking] Track " << ftn
                     << " newly SELECTED for publisher-session " << session.get()
                     << " (value=" << newKey.value << ")";
          publisherNotifications.emplace_back(session, info.forward);
        }
      } else {
        // Viewer: use shared threshold
        if (needViewerNotification) {
          viewerBatch.emplace_back(session, info.forward);
        }
      }
    }

    // Send publisher notifications
    for (const auto& [session, forward] : publisherNotifications) {
      if (session && onSelected_) {
        onSelected_(ftn, session, forward);
      }
    }

    // Send batch notification for viewers
    if (!viewerBatch.empty()) {
      XLOG(DBG4) << "[PropertyRanking] Track " << ftn
                 << " batch SELECTED for " << viewerBatch.size() << " viewers";
      onBatchSelected_(ftn, viewerBatch);
    }

    // Handle track that fell out of shared top N (for viewers)
    if (wasInTopN != nowInTopN && nowInTopN) {
      uint64_t count = 0;
      for (auto& [key, rankedEntry] : rankedTracks_) {
        if (count == n) {
          auto stIt = topNGroup.trackStates.find(rankedEntry.ftn);
          if (stIt != topNGroup.trackStates.end() &&
              stIt->second == TrackState::Selected) {
            stIt->second = TrackState::Deselected;
            topNGroup.deselectedQueue.push_back(rankedEntry.ftn);
            trimDeselectedQueue(topNGroup);
          }
          break;
        }
        count++;
      }
    }

    // Handle track that entered shared top N (for viewers) when one left
    if (wasInTopN != nowInTopN && !nowInTopN) {
      uint64_t count = 0;
      for (auto& [key, rankedEntry] : rankedTracks_) {
        if (count == n - 1) {
          auto stIt = topNGroup.trackStates.find(rankedEntry.ftn);
          if (stIt == topNGroup.trackStates.end() ||
              stIt->second != TrackState::Selected) {
            topNGroup.trackStates[rankedEntry.ftn] = TrackState::Selected;

            auto& dq = topNGroup.deselectedQueue;
            dq.erase(std::remove(dq.begin(), dq.end(), rankedEntry.ftn), dq.end());

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

    if (stats_.enabled) {
      stats_.evictionsTriggered++;
    }

    XLOG(DBG4) << "[PropertyRanking] Evicting track " << evicted
               << " from TopNGroup maxSelected=" << topNGroup.maxSelected
               << " (deselectedQueue exceeded maxDeselected=" << maxDeselected_ << ")";

    // Use iteration guard - callbacks must not add/remove sessions
    IterationGuard guard(*this);
    for (const auto& [session, info] : topNGroup.sessions) {
      if (session && onEvicted_) {
        XLOG(DBG4) << "[PropertyRanking] Notifying session " << session.get()
                   << " of eviction: " << evicted;
        onEvicted_(evicted, session);
      }
    }
  }
}

void PropertyRanking::notifyTrackSelected(
    const moxygen::FullTrackName& ftn,
    TopNGroup& topNGroup) {
  // Batch notify viewers, individual for publishers (with self-exclusion)
  std::vector<std::pair<std::shared_ptr<moxygen::MoQSession>, bool>> viewerBatch;
  for (const auto& [session, info] : topNGroup.sessions) {
    // Skip self-tracks for publishers
    if (info.isSelfTrack(ftn)) {
      if (stats_.enabled) {
        stats_.selfExclusionSkips++;
      }
      continue;
    }
    if (!info.isPublisher()) {
      viewerBatch.emplace_back(session, info.forward);
    } else if (session && onSelected_) {
      onSelected_(ftn, session, info.forward);
    }
  }
  if (!viewerBatch.empty()) {
    onBatchSelected_(ftn, viewerBatch);
  }
}

std::optional<RankKey> PropertyRanking::computeWaterlineKey(
    const SessionInfo& info,
    uint64_t maxSelected) const {
  if (!info.isPublisher()) {
    XLOG(DBG5) << "[Waterline] Not a publisher, no waterline needed";
    return std::nullopt;
  }

  XLOG(DBG5) << "[Waterline] Computing waterline for publisher with "
             << info.publishedTracks.size() << " self-tracks, maxSelected="
             << maxSelected;

  // Find the Nth non-self track
  uint64_t nonSelfCount = 0;
  for (const auto& [key, entry] : rankedTracks_) {
    bool isSelf = info.isSelfTrack(entry.ftn);
    XLOG(DBG5) << "[Waterline] Track " << entry.ftn
               << " value=" << key.value
               << " isSelf=" << isSelf;
    if (!isSelf) {
      nonSelfCount++;
      if (nonSelfCount == maxSelected) {
        XLOG(DBG5) << "[Waterline] Found waterline at track " << entry.ftn
                   << " value=" << key.value
                   << " (this is non-self track #" << nonSelfCount << ")";
        return key;
      }
    }
  }

  XLOG(DBG5) << "[Waterline] Only " << nonSelfCount
             << " non-self tracks exist (need " << maxSelected << ")";
  // Fewer than N non-self tracks exist
  return std::nullopt;
}

bool PropertyRanking::shouldSelectForSession(
    const moxygen::FullTrackName& ftn,
    const RankKey& key,
    const SessionInfo& info,
    uint64_t maxSelected) const {
  // Self tracks are never selected
  if (info.isSelfTrack(ftn)) {
    XLOG(DBG5) << "[Selection] Track " << ftn
               << " REJECTED: self-exclusion (publisher's own track)";
    return false;
  }

  // For viewers (non-publishers), use shared threshold
  if (!info.isPublisher()) {
    uint64_t rank = getRank(key);
    bool selected = rank < maxSelected;
    XLOG(DBG5) << "[Selection] Track " << ftn
               << " for viewer: rank=" << rank
               << " maxSelected=" << maxSelected
               << " -> " << (selected ? "SELECTED" : "rejected");
    return selected;
  }

  // For publisher-subscribers, use waterline
  if (!info.waterlineKey) {
    // No waterline means fewer than N non-self tracks exist
    // Select all non-self tracks
    XLOG(DBG5) << "[Selection] Track " << ftn
               << " for publisher: no waterline -> SELECTED (all non-self)";
    return true;
  }

  // Select if key >= waterline (higher value = better rank)
  bool selected = key > *info.waterlineKey || key == *info.waterlineKey;
  XLOG(DBG5) << "[Selection] Track " << ftn
             << " for publisher: value=" << key.value
             << " waterline=" << info.waterlineKey->value
             << " -> " << (selected ? "SELECTED" : "rejected");
  return selected;
}

void PropertyRanking::addPublishedTrackToSession(
    uint64_t maxSelected,
    const std::shared_ptr<moxygen::MoQSession>& session,
    const moxygen::FullTrackName& ftn) {
  XLOG(DBG4) << "[Self-exclusion] addPublishedTrackToSession: session="
             << session.get() << " maxSelected=" << maxSelected
             << " ftn=" << ftn;

  auto groupIt = topNGroups_.find(maxSelected);
  if (groupIt == topNGroups_.end()) {
    XLOG(DBG4) << "[Self-exclusion] TopNGroup maxSelected=" << maxSelected
               << " NOT FOUND, topNGroups_.size()=" << topNGroups_.size();
    return;
  }

  auto sessionIt = groupIt->second.sessions.find(session);
  if (sessionIt == groupIt->second.sessions.end()) {
    XLOG(DBG4) << "[Self-exclusion] Session " << session.get()
               << " NOT FOUND in TopNGroup maxSelected=" << maxSelected
               << " sessions.size()=" << groupIt->second.sessions.size();
    return;
  }

  XLOG(DBG4) << "[Self-exclusion] Adding self-track " << ftn
             << " for session " << session.get();

  auto& info = sessionIt->second;
  info.publishedTracks.insert(ftn);

  // Invalidate and recompute waterline
  info.waterlineValid = false;
  auto oldWaterline = info.waterlineKey;
  info.waterlineKey = computeWaterlineKey(info, maxSelected);
  info.waterlineValid = true;

  // If waterline changed, we may need to notify of new track
  if (info.waterlineKey != oldWaterline) {
    // Find the track that just became selected due to self-track being added
    for (const auto& [key, entry] : rankedTracks_) {
      if (info.isSelfTrack(entry.ftn)) {
        continue;
      }
      if (shouldSelectForSession(entry.ftn, key, info, maxSelected)) {
        if (onSelected_) {
          onSelected_(entry.ftn, session, info.forward);
        }
      }
    }
  }
}

void PropertyRanking::removePublishedTrackFromSession(
    uint64_t maxSelected,
    const std::shared_ptr<moxygen::MoQSession>& session,
    const moxygen::FullTrackName& ftn) {
  auto groupIt = topNGroups_.find(maxSelected);
  if (groupIt == topNGroups_.end()) {
    return;
  }

  auto sessionIt = groupIt->second.sessions.find(session);
  if (sessionIt == groupIt->second.sessions.end()) {
    return;
  }

  auto& info = sessionIt->second;
  info.publishedTracks.erase(ftn);

  // Invalidate and recompute waterline
  info.waterlineValid = false;
  info.waterlineKey = computeWaterlineKey(info, maxSelected);
  info.waterlineValid = true;
}

} // namespace openmoq::moqx
