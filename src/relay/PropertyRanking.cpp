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
    SelectCallback onSelected,
    EvictCallback onEvicted)
    : propertyType_(propertyType),
      maxDeselected_(maxDeselected),
      onSelected_(std::move(onSelected)),
      onEvicted_(std::move(onEvicted)) {}

PropertyRanking::PropertyRanking(
    uint64_t propertyType,
    uint64_t maxDeselected,
    SelectCallback onSelected,
    EvictCallback onEvicted,
    Tick idleThreshold,
    std::chrono::milliseconds sweepInterval,
    folly::HHWheelTimer& timer,
    GetLastActivityFn getLastActivity)
    : propertyType_(propertyType),
      maxDeselected_(maxDeselected),
      onSelected_(std::move(onSelected)),
      onEvicted_(std::move(onEvicted)),
      idleThreshold_(idleThreshold),
      sweepInterval_(sweepInterval),
      timer_(&timer),
      getLastActivity_(std::move(getLastActivity)) {}

PropertyRanking::~PropertyRanking() {
  stopIdleSweep();
}

void PropertyRanking::registerTrack(
    const moxygen::FullTrackName& ftn,
    std::optional<uint64_t> initialValue,
    std::weak_ptr<moxygen::MoQSession> publisher) {
  if (metrics_.enabled) {
    metrics_.tracksRegistered++;
  }

  // Check if already registered
  if (tracks_.find(ftn) != tracks_.end()) {
    XLOG(WARN) << "Track already registered: " << ftn;
    return;
  }

  // Use initial value or 0 as default
  uint64_t value = initialValue.value_or(0);
  RankKey key{-static_cast<int64_t>(value), nextSeq_++};

  // Insert into sorted map
  auto [iter, inserted] = ranked_.emplace(
      key, RankedEntry{.ftn = ftn, .publisher = std::move(publisher)});

  // Store iterator for O(1) lookup
  tracks_[ftn] = TrackEntry{.rankIter = iter, .cachedRank = UINT64_MAX};

  // Invalidate rank cache since structure changed
  invalidateRankCache();

  XLOG(DBG4) << "Registered track " << ftn << " with value " << value
             << " at rank " << getRank(key);

  // Check if this track should be selected for any group
  uint64_t rank = getRank(key);
  for (auto& [n, group] : groups_) {
    if (rank < n) {
      // Track is in top N, mark as selected
      group.trackStates[ftn] = TrackState::Selected;

      // Notify sessions in this group (respecting self-exclusion)
      // Separate viewers (can batch) from publishers (need individual waterline checks)
      std::vector<std::pair<std::shared_ptr<moxygen::MoQSession>, bool>> viewerBatch;
      std::vector<std::pair<std::shared_ptr<moxygen::MoQSession>, bool>> publisherNotifications;
      for (const auto& [session, info] : group.sessions) {
        if (info.isSelfTrack(ftn)) {
          XLOG(DBG4) << "[registerTrack] Skipping self-track " << ftn
                     << " for session " << session.get();
          if (metrics_.enabled) {
            metrics_.selfExclusionSkips++;
          }
          continue;
        }
        if (info.isPublisher()) {
          publisherNotifications.emplace_back(session, info.forward);
        } else {
          viewerBatch.emplace_back(session, info.forward);
        }
      }

      std::chrono::steady_clock::time_point notifyStart;
      if (metrics_.timingEnabled) {
        notifyStart = std::chrono::steady_clock::now();
      }

      // Batch notify viewers if callback available
      if (onBatchSelected_ && !viewerBatch.empty()) {
        if (metrics_.enabled) {
          metrics_.selectionsTriggered += viewerBatch.size();
        }
        onBatchSelected_(ftn, viewerBatch);
      } else {
        for (const auto& [session, forward] : viewerBatch) {
          if (session && onSelected_) {
            if (metrics_.enabled) {
              metrics_.selectionsTriggered++;
            }
            onSelected_(ftn, session, forward);
          }
        }
      }

      // Notify publishers individually
      for (const auto& [session, forward] : publisherNotifications) {
        if (session && onSelected_) {
          if (metrics_.enabled) {
            metrics_.selectionsTriggered++;
          }
          onSelected_(ftn, session, forward);
        }
      }

      if (metrics_.timingEnabled) {
        metrics_.notifyTimeNs += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - notifyStart).count();
      }
    } else if (rank < poolBoundary_) {
      // Track is in deselected pool
      group.trackStates[ftn] = TrackState::Deselected;
      group.deselectedQueue.push_back(ftn);
      trimDeselectedQueue(group);
    }
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
  RankKey newKey{-static_cast<int64_t>(value), oldKey.arrivalSeq};

  // No change in value
  if (oldKey.negValue == newKey.negValue) {
    return;
  }

  // FAST PATH: value change doesn't cross any threshold
  if (!crossesThreshold(oldKey, newKey)) {
    // Just update position in sorted map using extract/insert
    auto rankedEntry = std::move(entry.rankIter->second);
    ranked_.erase(entry.rankIter);
    auto [newIter, _] = ranked_.emplace(newKey, std::move(rankedEntry));
    entry.rankIter = newIter;
    // Invalidate cache - positions shifted
    invalidateRankCache();
    return;
  }

  // SLOW PATH: value crossed a threshold, recompute groups
  auto rankedEntry = std::move(entry.rankIter->second);
  ranked_.erase(entry.rankIter);
  auto [newIter, _] = ranked_.emplace(newKey, std::move(rankedEntry));
  entry.rankIter = newIter;

  // Invalidate cache - positions shifted
  invalidateRankCache();

  recomputeGroups(ftn, oldKey, newKey);
}

void PropertyRanking::removeTrack(const moxygen::FullTrackName& ftn) {
  auto it = tracks_.find(ftn);
  if (it == tracks_.end()) {
    return;
  }

  auto& entry = it->second;
  RankKey oldKey = entry.rankIter->first;
  uint64_t oldRank = getRank(oldKey);

  // Remove from ranked map
  ranked_.erase(entry.rankIter);
  tracks_.erase(it);

  // Invalidate rank cache since structure changed
  invalidateRankCache();

  XLOG(DBG4) << "Removed track " << ftn << " from rank " << oldRank;

  // Update groups - removed track may cause promotions
  for (auto& [n, group] : groups_) {
    auto stateIt = group.trackStates.find(ftn);
    if (stateIt == group.trackStates.end()) {
      continue;
    }

    bool wasSelected = stateIt->second == TrackState::Selected;
    group.trackStates.erase(stateIt);

    // Remove from deselected queue if present
    auto& dq = group.deselectedQueue;
    dq.erase(std::remove(dq.begin(), dq.end(), ftn), dq.end());

    if (wasSelected) {
      // Copy sessions to avoid iterator invalidation during callback
      std::vector<std::pair<std::shared_ptr<moxygen::MoQSession>, bool>> sessionsToNotify;
      for (const auto& [session, info] : group.sessions) {
        sessionsToNotify.emplace_back(session, info.forward);
      }

      // Need to promote next track from deselected queue or ranked list
      // First try deselected queue
      if (!group.deselectedQueue.empty()) {
        auto promoted = group.deselectedQueue.front();
        group.deselectedQueue.pop_front();
        group.trackStates[promoted] = TrackState::Selected;

        // Notify sessions
        for (const auto& [session, forward] : sessionsToNotify) {
          if (session && onSelected_) {
            onSelected_(promoted, session, forward);
          }
        }
      } else {
        // Find next track from ranked list that isn't already selected
        uint64_t count = 0;
        for (auto& [key, rankedEntry] : ranked_) {
          auto stIt = group.trackStates.find(rankedEntry.ftn);
          if (stIt != group.trackStates.end() &&
              stIt->second == TrackState::Selected) {
            count++;
            continue;
          }
          // This track is not selected, promote it if we have room
          if (count < n) {
            group.trackStates[rankedEntry.ftn] = TrackState::Selected;
            for (const auto& [session, forward] : sessionsToNotify) {
              if (session && onSelected_) {
                onSelected_(rankedEntry.ftn, session, forward);
              }
            }
            break;
          }
          count++;
        }
      }
    }
  }
}

TopNGroup& PropertyRanking::getOrCreateGroup(uint64_t maxSelected) {
  bool wasEmpty = groups_.empty();
  auto [it, inserted] = groups_.try_emplace(maxSelected);
  if (inserted) {
    it->second.maxSelected = maxSelected;
    updatePoolBoundary();

    // Initialize track states for existing tracks
    uint64_t rank = 0;
    for (auto& [key, entry] : ranked_) {
      if (rank < maxSelected) {
        it->second.trackStates[entry.ftn] = TrackState::Selected;
      } else if (rank < poolBoundary_) {
        it->second.trackStates[entry.ftn] = TrackState::Deselected;
        it->second.deselectedQueue.push_back(entry.ftn);
      }
      rank++;
    }
    trimDeselectedQueue(it->second);

    // Start idle sweep when first group is created
    if (wasEmpty) {
      startIdleSweep();
    }
  }
  return it->second;
}

void PropertyRanking::addSessionToGroup(
    uint64_t maxSelected,
    std::shared_ptr<moxygen::MoQSession> session,
    bool forward,
    std::vector<moxygen::FullTrackName> publishedTracks) {
  XLOG(DBG4) << "[PropertyRanking] Adding session " << session.get()
             << " to group maxSelected=" << maxSelected
             << " forward=" << forward
             << " publishedTracks=" << publishedTracks.size();

  auto& group = getOrCreateGroup(maxSelected);
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
    info.waterlineValid = true;  // Mark as valid after computation
    if (info.waterlineKey) {
      XLOG(DBG4) << "[PropertyRanking] Session " << session.get()
                 << " waterline computed: negValue=" << info.waterlineKey->negValue
                 << " (value=" << -info.waterlineKey->negValue << ")"
                 << " arrivalSeq=" << info.waterlineKey->arrivalSeq;
    } else {
      XLOG(DBG4) << "[PropertyRanking] Session " << session.get()
                 << " waterline: none (fewer than " << maxSelected
                 << " non-self tracks)";
    }
  }

  group.sessions[session] = std::move(info);

  // Notify session of tracks it should receive (respecting self-exclusion)
  const auto& sessionInfo = group.sessions[session];
  for (auto& [key, entry] : ranked_) {
    if (shouldSelectForSession(entry.ftn, key, sessionInfo, maxSelected)) {
      XLOG(DBG4) << "[PropertyRanking] Selecting track " << entry.ftn
                 << " for session " << session.get()
                 << " (value=" << -key.negValue << ")";
      if (onSelected_) {
        onSelected_(entry.ftn, session, sessionInfo.forward);
      }
    }
  }
}

void PropertyRanking::removeSessionFromGroup(
    uint64_t maxSelected,
    const std::shared_ptr<moxygen::MoQSession>& session) {
  auto it = groups_.find(maxSelected);
  if (it == groups_.end()) {
    return;
  }

  it->second.sessions.erase(session);

  // Remove group if no sessions remain
  if (it->second.sessions.empty()) {
    removeGroup(maxSelected);
  }
}

void PropertyRanking::removeGroup(uint64_t maxSelected) {
  groups_.erase(maxSelected);
  updatePoolBoundary();

  // Stop idle sweep when last group is removed
  if (groups_.empty()) {
    stopIdleSweep();
  }
}

uint64_t PropertyRanking::getRank(const RankKey& key) const {
  auto it = ranked_.find(key);
  if (it == ranked_.end()) {
    return UINT64_MAX;
  }
  // Rebuild cache if invalidated, then return O(1) lookup
  rebuildRankCacheIfNeeded();
  auto trackIt = tracks_.find(it->second.ftn);
  if (trackIt != tracks_.end()) {
    return trackIt->second.cachedRank;
  }
  // Fallback (shouldn't happen if data structures are consistent)
  return std::distance(ranked_.begin(), it);
}

void PropertyRanking::rebuildRankCacheIfNeeded() const {
  if (rankCacheValid_) {
    return;
  }
  uint64_t rank = 0;
  for (const auto& [key, entry] : ranked_) {
    auto it = tracks_.find(entry.ftn);
    if (it != tracks_.end()) {
      const_cast<TrackEntry&>(it->second).cachedRank = rank;
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
  if (groups_.empty()) {
    return false;
  }

  // Get positions in sorted order
  uint64_t oldRank = oldKey ? getRank(*oldKey) : UINT64_MAX;

  // For newKey, we need to find where it would be inserted
  // Since we already updated ranked_, we can use getRank directly
  uint64_t newRank = getRank(newKey);

  // OPTIMIZATION: Early exit if both ranks are far from any threshold
  // If both are beyond pool boundary, no threshold can be crossed
  if (oldRank >= poolBoundary_ && newRank >= poolBoundary_) {
    return false;
  }

  uint64_t minRank = std::min(oldRank, newRank);
  uint64_t maxRank = std::max(oldRank, newRank);

  // Check if crossed pool boundary
  bool oldInPool = oldRank < poolBoundary_;
  bool newInPool = newRank < poolBoundary_;
  if (oldInPool != newInPool) {
    return true;
  }

  // OPTIMIZATION: O(log G) check using sorted thresholds
  // Find first threshold > minRank, check if it's <= maxRank
  auto it = std::upper_bound(sortedThresholds_.begin(), sortedThresholds_.end(), minRank);
  if (it != sortedThresholds_.end() && *it <= maxRank) {
    return true;  // At least one threshold is crossed
  }

  return false;
}

void PropertyRanking::recomputeGroups(
    const moxygen::FullTrackName& ftn,
    std::optional<RankKey> oldKey,
    const RankKey& newKey) {
  uint64_t oldRank = oldKey ? getRank(*oldKey) : UINT64_MAX;
  uint64_t newRank = getRank(newKey);

  XLOG(DBG4) << "recomputeGroups: " << ftn << " moved from rank " << oldRank
             << " to " << newRank;

  for (auto& [n, group] : groups_) {
    bool wasInTopN = oldRank < n;
    bool nowInTopN = newRank < n;

    // Update shared track state (for viewers)
    if (wasInTopN != nowInTopN) {
      if (nowInTopN) {
        group.trackStates[ftn] = TrackState::Selected;
        auto& dq = group.deselectedQueue;
        dq.erase(std::remove(dq.begin(), dq.end(), ftn), dq.end());
      } else {
        group.trackStates[ftn] = TrackState::Deselected;
        group.deselectedQueue.push_back(ftn);
        trimDeselectedQueue(group);
      }
    }

    // OPTIMIZATION: Batch viewer notifications
    std::vector<std::pair<std::shared_ptr<moxygen::MoQSession>, bool>> viewerBatch;
    bool needViewerNotification = !wasInTopN && nowInTopN;

    // Collect publisher notifications (need to copy since we may modify waterlines)
    std::vector<std::pair<std::shared_ptr<moxygen::MoQSession>, bool>> publisherNotifications;

    // Handle per-session notifications (with self-exclusion)
    for (auto& [session, info] : group.sessions) {
      if (info.isPublisher()) {
        // Skip self tracks entirely (no notification needed)
        if (info.isSelfTrack(ftn)) {
          XLOG(DBG5) << "[PropertyRanking] Skipping self-track " << ftn
                     << " for session " << session.get();
          // OPTIMIZATION: Only recompute waterline if self-track crossed N boundary
          // Self-track moving within top-N or outside top-N doesn't change waterline
          bool selfTrackCrossedN = (oldRank < n) != (newRank < n);
          if (!info.waterlineValid || selfTrackCrossedN) {
            auto oldWaterline = info.waterlineKey;
            info.waterlineKey = computeWaterlineKey(info, n);
            info.waterlineValid = true;
            if (oldWaterline != info.waterlineKey) {
              XLOG(DBG4) << "[PropertyRanking] Waterline changed for session "
                         << session.get() << " (self-track crossed N): "
                         << (oldWaterline ? std::to_string(-oldWaterline->negValue) : "none")
                         << " -> "
                         << (info.waterlineKey ? std::to_string(-info.waterlineKey->negValue) : "none");
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
                     << " (value=" << -newKey.negValue << ")";
          publisherNotifications.emplace_back(session, info.forward);
        }
      } else {
        // Viewer: use shared threshold
        if (needViewerNotification) {
          if (onBatchSelected_) {
            // Collect for batch notification
            viewerBatch.emplace_back(session, info.forward);
          } else {
            XLOG(DBG4) << "[PropertyRanking] Track " << ftn
                       << " newly SELECTED for viewer-session " << session.get()
                       << " (entered top-" << n << ")";
            viewerBatch.emplace_back(session, info.forward);
          }
        }
      }
    }

    // Send notifications outside the iteration loop to avoid iterator issues
    for (const auto& [session, forward] : publisherNotifications) {
      if (session && onSelected_) {
        onSelected_(ftn, session, forward);
      }
    }
    for (const auto& [session, forward] : viewerBatch) {
      if (session && onSelected_ && !onBatchSelected_) {
        onSelected_(ftn, session, forward);
      }
    }

    // OPTIMIZATION: Send batch notification for viewers
    if (onBatchSelected_ && !viewerBatch.empty()) {
      XLOG(DBG4) << "[PropertyRanking] Track " << ftn
                 << " batch SELECTED for " << viewerBatch.size() << " viewers";
      onBatchSelected_(ftn, viewerBatch);
    }

    // Handle track that fell out of shared top N (for viewers)
    if (wasInTopN != nowInTopN && nowInTopN) {
      uint64_t count = 0;
      for (auto& [key, rankedEntry] : ranked_) {
        if (count == n) {
          auto stIt = group.trackStates.find(rankedEntry.ftn);
          if (stIt != group.trackStates.end() &&
              stIt->second == TrackState::Selected) {
            stIt->second = TrackState::Deselected;
            group.deselectedQueue.push_back(rankedEntry.ftn);
            trimDeselectedQueue(group);
          }
          break;
        }
        count++;
      }
    }

    // Handle track that entered shared top N (for viewers) when one left
    if (wasInTopN != nowInTopN && !nowInTopN) {
      uint64_t count = 0;
      for (auto& [key, rankedEntry] : ranked_) {
        if (count == n - 1) {
          auto stIt = group.trackStates.find(rankedEntry.ftn);
          if (stIt == group.trackStates.end() ||
              stIt->second != TrackState::Selected) {
            group.trackStates[rankedEntry.ftn] = TrackState::Selected;

            auto& dq = group.deselectedQueue;
            dq.erase(std::remove(dq.begin(), dq.end(), rankedEntry.ftn), dq.end());

            // Copy viewer sessions before notifying
            std::vector<std::pair<std::shared_ptr<moxygen::MoQSession>, bool>> viewersToNotify;
            for (const auto& [session, sessionInfo] : group.sessions) {
              if (!sessionInfo.isPublisher()) {
                viewersToNotify.emplace_back(session, sessionInfo.forward);
              }
            }
            for (const auto& [session, forward] : viewersToNotify) {
              if (session && onSelected_) {
                onSelected_(rankedEntry.ftn, session, forward);
              }
            }
          }
          break;
        }
        count++;
      }
    }
  }
}

void PropertyRanking::updatePoolBoundary() {
  uint64_t maxN = 0;
  sortedThresholds_.clear();
  sortedThresholds_.reserve(groups_.size());
  for (const auto& [n, group] : groups_) {
    maxN = std::max(maxN, n);
    sortedThresholds_.push_back(n);
  }
  std::sort(sortedThresholds_.begin(), sortedThresholds_.end());
  poolBoundary_ = maxN + maxDeselected_;
}

void PropertyRanking::trimDeselectedQueue(TopNGroup& group) {
  while (group.deselectedQueue.size() > maxDeselected_) {
    auto evicted = group.deselectedQueue.front();
    group.deselectedQueue.pop_front();
    group.trackStates.erase(evicted);

    XLOG(DBG4) << "[PropertyRanking] Evicting track " << evicted
               << " from group maxSelected=" << group.maxSelected
               << " (deselectedQueue exceeded maxDeselected=" << maxDeselected_ << ")";

    // Copy sessions before notifying to avoid iterator issues
    std::vector<std::shared_ptr<moxygen::MoQSession>> sessionsToNotify;
    for (const auto& [session, info] : group.sessions) {
      sessionsToNotify.push_back(session);
    }
    for (const auto& session : sessionsToNotify) {
      if (session && onEvicted_) {
        XLOG(DBG4) << "[PropertyRanking] Notifying session " << session.get()
                   << " of eviction: " << evicted;
        onEvicted_(evicted, session);
      }
    }
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
  for (const auto& [key, entry] : ranked_) {
    bool isSelf = info.isSelfTrack(entry.ftn);
    XLOG(DBG5) << "[Waterline] Track " << entry.ftn
               << " value=" << -key.negValue
               << " isSelf=" << isSelf;
    if (!isSelf) {
      nonSelfCount++;
      if (nonSelfCount == maxSelected) {
        XLOG(DBG5) << "[Waterline] Found waterline at track " << entry.ftn
                   << " value=" << -key.negValue
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

  // Select if key <= waterline (remember: lower negValue = higher rank)
  bool selected = key < *info.waterlineKey || key == *info.waterlineKey;
  XLOG(DBG5) << "[Selection] Track " << ftn
             << " for publisher: value=" << -key.negValue
             << " waterline=" << -info.waterlineKey->negValue
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

  auto groupIt = groups_.find(maxSelected);
  if (groupIt == groups_.end()) {
    XLOG(DBG4) << "[Self-exclusion] Group maxSelected=" << maxSelected
               << " NOT FOUND, groups_.size()=" << groups_.size();
    return;
  }

  auto sessionIt = groupIt->second.sessions.find(session);
  if (sessionIt == groupIt->second.sessions.end()) {
    XLOG(DBG4) << "[Self-exclusion] Session " << session.get()
               << " NOT FOUND in group maxSelected=" << maxSelected
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
    // This happens when a self-track was in top N, pushing a non-self track out
    // Now that track should be included
    for (const auto& [key, entry] : ranked_) {
      if (info.isSelfTrack(entry.ftn)) {
        continue;
      }
      if (shouldSelectForSession(entry.ftn, key, info, maxSelected)) {
        // Check if this track wasn't previously selected for this session
        // We need to track per-session selected state for this
        // For now, just notify - the session should handle duplicates
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
  auto groupIt = groups_.find(maxSelected);
  if (groupIt == groups_.end()) {
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

void PropertyRanking::startIdleSweep() {
  if (!timer_ || sweepRunning_ || sweepInterval_.count() == 0) {
    return;
  }
  sweepRunning_ = true;
  timer_->scheduleTimeout(this, sweepInterval_);
  XLOG(DBG4) << "Started idle sweep timer with interval "
             << sweepInterval_.count() << "ms";
}

void PropertyRanking::stopIdleSweep() {
  if (!sweepRunning_) {
    return;
  }
  sweepRunning_ = false;
  cancelTimeout();
  XLOG(DBG4) << "Stopped idle sweep timer";
}

void PropertyRanking::timeoutExpired() noexcept {
  XLOG(DBG4) << "Idle sweep timer fired";
  sweepIdle();

  // Reschedule if still running
  if (sweepRunning_ && timer_) {
    timer_->scheduleTimeout(this, sweepInterval_);
  }
}

void PropertyRanking::sweepIdle() {
  if (!getLastActivity_ || idleThreshold_ == 0) {
    return;
  }

  Tick currentTick = getCurrentTick();

  // Iterate all groups and check selected tracks for idleness
  for (auto& [n, group] : groups_) {
    std::vector<moxygen::FullTrackName> toDeselect;

    // Find idle selected tracks
    for (const auto& [ftn, state] : group.trackStates) {
      if (state != TrackState::Selected) {
        continue;
      }

      Tick lastActivity = getLastActivity_(ftn);
      if (currentTick > lastActivity &&
          (currentTick - lastActivity) > idleThreshold_) {
        toDeselect.push_back(ftn);
        XLOG(DBG4) << "Track " << ftn << " is idle (last activity: "
                   << lastActivity << ", current: " << currentTick << ")";
      }
    }

    // Demote idle tracks
    for (const auto& ftn : toDeselect) {
      // Move to deselected queue
      group.trackStates[ftn] = TrackState::Deselected;
      group.deselectedQueue.push_back(ftn);

      // Find next track to promote (from deselected queue or ranked list)
      moxygen::FullTrackName promoted;
      bool foundPromotion = false;

      // Try deselected queue first
      for (auto it = group.deselectedQueue.begin();
           it != group.deselectedQueue.end();
           ++it) {
        if (*it != ftn) {
          promoted = *it;
          group.deselectedQueue.erase(it);
          foundPromotion = true;
          break;
        }
      }

      if (!foundPromotion) {
        // Find next unselected track from ranked list
        uint64_t selectedCount = 0;
        for (const auto& [key, entry] : ranked_) {
          auto stIt = group.trackStates.find(entry.ftn);
          if (stIt != group.trackStates.end() &&
              stIt->second == TrackState::Selected) {
            selectedCount++;
            continue;
          }
          // This track is not selected
          if (selectedCount < n && entry.ftn != ftn) {
            promoted = entry.ftn;
            foundPromotion = true;
            break;
          }
        }
      }

      if (foundPromotion) {
        group.trackStates[promoted] = TrackState::Selected;

        // Copy sessions before notifying to avoid iterator issues
        std::vector<std::pair<std::shared_ptr<moxygen::MoQSession>, bool>> sessionsToNotify;
        for (const auto& [session, info] : group.sessions) {
          // Skip self-tracks for publisher-subscribers
          if (info.isPublisher() && info.isSelfTrack(promoted)) {
            continue;
          }
          sessionsToNotify.emplace_back(session, info.forward);
        }
        // Notify sessions of the promotion
        for (const auto& [session, forward] : sessionsToNotify) {
          if (session && onSelected_) {
            onSelected_(promoted, session, forward);
          }
        }
      }

      // Trim deselected queue if needed
      trimDeselectedQueue(group);
    }
  }
}

} // namespace openmoq::moqx
