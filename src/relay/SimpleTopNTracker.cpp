/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/SimpleTopNTracker.h"
#include "relay/TopNEventLog.h"

#include <folly/logging/xlog.h>

#include <algorithm>

namespace openmoq::moqx {

// ----------------------------------------------------------------------------
// SimpleTopNTracker - just maintains global sorted snapshot
// ----------------------------------------------------------------------------

SimpleTopNTracker::SimpleTopNTracker(
    uint64_t propertyType,
    std::chrono::milliseconds idleTimeout,
    std::chrono::milliseconds sweepThrottle,
    GetLastActivityFn getLastActivity,
    SnapshotChangedCallback onSnapshotChanged
)
    : propertyType_(propertyType), idleTimeout_(idleTimeout), sweepThrottle_(sweepThrottle),
      getLastActivity_(std::move(getLastActivity)), onSnapshotChanged_(std::move(onSnapshotChanged)),
      snapshot_(std::make_shared<Snapshot>()) {}

void SimpleTopNTracker::registerTrack(
    const moxygen::FullTrackName& ftn,
    std::optional<uint64_t> initialPropertyValue,
    std::shared_ptr<moxygen::MoQSession> publisher
) {
  std::lock_guard<std::mutex> lock(writeMutex_);

  if (trackIndex_.find(ftn) != trackIndex_.end()) {
    XLOG(WARN) << "Track already registered: " << ftn;
    return;
  }

  uint64_t value = initialPropertyValue.value_or(0);
  uint64_t seq = nextSeq_++;

  auto* pubRaw = publisher.get();
  trackIndex_[ftn] = TrackInfo{
      .propertyValue = value, .arrivalSeq = seq, .publisher = std::move(publisher)};

  // Update publisher track count for N+X snapshot sizing
  publisherTrackCount_[pubRaw]++;
  updateMaxTracksPerPublisher();

  auto oldSnapshot = std::atomic_load(&snapshot_);
  rebuildSnapshot();
  auto newSnapshot = std::atomic_load(&snapshot_);

  XLOG(DBG4) << "Registered track " << ftn << " with value " << value;

  TopNEventLog::instance().logTrackRegistered(ftn, value, pubRaw);

  if (onSnapshotChanged_) {
    onSnapshotChanged_(oldSnapshot, newSnapshot, std::nullopt);
  }
}

void SimpleTopNTracker::updateSortValue(const moxygen::FullTrackName& ftn, uint64_t value) {
  std::lock_guard<std::mutex> lock(writeMutex_);

  auto it = trackIndex_.find(ftn);
  if (it == trackIndex_.end()) {
    XLOG(DBG4) << "updateSortValue: track not registered " << ftn;
    return;
  }

  if (it->second.propertyValue == value) {
    return;
  }

  uint64_t oldValue = it->second.propertyValue;
  it->second.propertyValue = value;

  XLOG(DBG4) << "Updated track " << ftn << " to value " << value;
  TopNEventLog::instance().logValueUpdated(ftn, oldValue, value, it->second.publisher.get());

  // Mark dirty — snapshot rebuild deferred until flush()
  dirty_.store(true, std::memory_order_release);
}

void SimpleTopNTracker::flush() {
  // Fast path: nothing to do
  if (!dirty_.load(std::memory_order_acquire)) {
    return;
  }

  std::lock_guard<std::mutex> lock(writeMutex_);

  // Re-check under lock (another thread may have flushed)
  if (!dirty_.load(std::memory_order_relaxed)) {
    return;
  }

  // Time-based coalescing: skip if we flushed recently
  if (flushInterval_.count() > 0) {
    auto now = std::chrono::steady_clock::now();
    if (lastFlushTime_ != std::chrono::steady_clock::time_point{} &&
        now - lastFlushTime_ < flushInterval_) {
      return;
    }
    lastFlushTime_ = now;
  }

  dirty_.store(false, std::memory_order_release);

  auto oldSnapshot = std::atomic_load(&snapshot_);
  rebuildSnapshot();
  auto newSnapshot = std::atomic_load(&snapshot_);

  if (onSnapshotChanged_) {
    onSnapshotChanged_(oldSnapshot, newSnapshot, std::nullopt);
  }

  sweepIdle();
}

void SimpleTopNTracker::removeTrack(const moxygen::FullTrackName& ftn) {
  std::lock_guard<std::mutex> lock(writeMutex_);

  auto it = trackIndex_.find(ftn);
  if (it == trackIndex_.end()) {
    return;
  }

  // Update publisher track count
  auto* pubRaw = it->second.publisher.get();
  auto countIt = publisherTrackCount_.find(pubRaw);
  if (countIt != publisherTrackCount_.end()) {
    if (--countIt->second == 0) {
      publisherTrackCount_.erase(countIt);
    }
    updateMaxTracksPerPublisher();
  }

  trackIndex_.erase(it);

  auto oldSnapshot = std::atomic_load(&snapshot_);
  rebuildSnapshot();
  auto newSnapshot = std::atomic_load(&snapshot_);

  XLOG(DBG4) << "Removed track " << ftn;

  TopNEventLog::instance().logTrackRemoved(ftn);

  if (onSnapshotChanged_) {
    onSnapshotChanged_(oldSnapshot, newSnapshot, ftn);
  }
}

void SimpleTopNTracker::addSession(
    uint64_t maxSelected,
    std::shared_ptr<moxygen::MoQSession> session,
    bool forward
) {
  std::lock_guard<std::mutex> lock(writeMutex_);

  XLOG(DBG4) << "addSession: maxSelected=" << maxSelected << " forward=" << forward;

  sessions_[session] = SessionInfo{.maxSelected = maxSelected, .forward = forward};
  updateMaxN();

  TopNEventLog::instance().logSubscriberRegistered(session.get(), maxSelected);

  // Trigger callback so relay can compute initial selection
  auto snapshot = std::atomic_load(&snapshot_);
  if (onSnapshotChanged_) {
    onSnapshotChanged_(snapshot, snapshot, std::nullopt);
  }
}

void SimpleTopNTracker::updateSessionForward(
    uint64_t maxSelected,
    const std::shared_ptr<moxygen::MoQSession>& session,
    bool forward
) {
  std::lock_guard<std::mutex> lock(writeMutex_);

  auto it = sessions_.find(session);
  if (it == sessions_.end()) {
    XLOG(WARN) << "updateSessionForward: session not found";
    return;
  }

  XLOG(DBG4) << "updateSessionForward: forward=" << it->second.forward << " -> " << forward;
  it->second.forward = forward;
}

void SimpleTopNTracker::removeSession(
    uint64_t maxSelected,
    const std::shared_ptr<moxygen::MoQSession>& session
) {
  std::lock_guard<std::mutex> lock(writeMutex_);

  sessions_.erase(session);
  updateMaxN();
}

void SimpleTopNTracker::sweepIdle() {
  if (idleTimeout_.count() == 0 || !getLastActivity_) {
    return;
  }

  auto now = std::chrono::steady_clock::now();

  if (sweepThrottle_.count() > 0 && lastSweepTime_ && now - *lastSweepTime_ < sweepThrottle_) {
    return;
  }
  lastSweepTime_ = now;

  auto snapshot = std::atomic_load(&snapshot_);
  uint8_t currentMaxN = maxN_.load(std::memory_order_relaxed);

  std::vector<moxygen::FullTrackName> toEvict;

  uint64_t rank = 0;
  for (const auto& track : *snapshot) {
    if (rank >= currentMaxN) {
      break;
    }

    auto lastActivity = getLastActivity_(track.ftn);
    bool neverPublished = (lastActivity == std::chrono::steady_clock::time_point{});

    if (neverPublished || now - lastActivity > idleTimeout_) {
      XLOG(DBG4) << "[SimpleTopNTracker] Idle eviction candidate: " << track.ftn
                 << (neverPublished ? " [never published]" : "");
      toEvict.push_back(track.ftn);
    }
    rank++;
  }

  for (const auto& ftn : toEvict) {
    auto it = trackIndex_.find(ftn);
    if (it != trackIndex_.end()) {
      trackIndex_.erase(it);

      auto oldSnapshot = std::atomic_load(&snapshot_);
      rebuildSnapshot();
      auto newSnapshot = std::atomic_load(&snapshot_);

      if (onSnapshotChanged_) {
        onSnapshotChanged_(oldSnapshot, newSnapshot, ftn);
      }
    }
  }
}

moxygen::MoQSession* SimpleTopNTracker::getPublisher(const moxygen::FullTrackName& ftn) const {
  std::lock_guard<std::mutex> lock(writeMutex_);
  auto it = trackIndex_.find(ftn);
  if (it != trackIndex_.end()) {
    return it->second.publisher.get();
  }
  return nullptr;
}

bool SimpleTopNTracker::shouldForward(
    const moxygen::FullTrackName& ftn,
    uint8_t subscriberN,
    const Snapshot& snapshot
) const {
  uint64_t rank = 0;
  for (const auto& track : snapshot) {
    if (track.ftn == ftn) {
      return rank < subscriberN;
    }
    if (++rank >= subscriberN) {
      break;
    }
  }
  return false;
}

bool SimpleTopNTracker::isInTopNWithSelfExclusion(
    const moxygen::FullTrackName& ftn,
    const std::shared_ptr<moxygen::MoQSession>& session,
    uint64_t n,
    const Snapshot& snapshot,
    uint8_t cachedLastSelfPosition
) const {
  // Pure subscriber fast path: no self-exclusion needed
  if (session == nullptr) {
    return shouldForward(ftn, static_cast<uint8_t>(n), snapshot);
  }

  // Find track's position in snapshot
  size_t trackPosition = UINT64_MAX;
  for (size_t i = 0; i < snapshot.size(); ++i) {
    if (snapshot[i].ftn == ftn) {
      trackPosition = i;
      break;
    }
  }

  // Track not in snapshot at all
  if (trackPosition == UINT64_MAX) {
    return false;
  }

  // O(1) fast rejection: if track is beyond N + lastSelfPosition,
  // it definitely can't be in subscriber's top-N non-self
  if (!mightBeInTopN(trackPosition, static_cast<uint8_t>(n), cachedLastSelfPosition)) {
    return false;
  }

  // Full scan needed: count non-self tracks up to this position
  auto* sessionRaw = session.get();
  uint64_t nonSelfCount = 0;

  for (size_t i = 0; i <= trackPosition && i < snapshot.size(); ++i) {
    // Skip self-tracks
    if (snapshot[i].publisherRaw == sessionRaw) {
      continue;
    }

    // Found the target track before reaching N non-self tracks
    if (snapshot[i].ftn == ftn) {
      return nonSelfCount < n;
    }

    ++nonSelfCount;
    // Already found N non-self tracks, target not among them
    if (nonSelfCount >= n) {
      return false;
    }
  }

  return false;
}

std::vector<moxygen::FullTrackName> SimpleTopNTracker::computeTopNForSession(
    const std::shared_ptr<moxygen::MoQSession>& session,
    uint64_t n,
    const Snapshot& snapshot
) const {
  std::vector<moxygen::FullTrackName> result;
  result.reserve(n);

  for (const auto& track : snapshot) {
    if (result.size() >= n) {
      break;
    }
    // Self-exclusion: skip tracks published by this session
    if (track.publisherRaw != session.get()) {
      result.push_back(track.ftn);
    }
  }

  return result;
}

void SimpleTopNTracker::rebuildSnapshot() {
  uint8_t currentMaxN = maxN_.load(std::memory_order_relaxed);
  if (currentMaxN == 0) {
    std::atomic_store(&snapshot_, std::make_shared<Snapshot>());
    return;
  }

  std::vector<TrackRank> allTracks;
  allTracks.reserve(trackIndex_.size());

  for (const auto& [ftn, info] : trackIndex_) {
    allTracks.push_back(TrackRank{
        .ftn = ftn,
        .propertyValue = info.propertyValue,
        .arrivalSeq = info.arrivalSeq,
        .lastUpdateTs = std::chrono::steady_clock::now(),
        .publisherRaw = info.publisher.get()});
  }

  std::sort(allTracks.begin(), allTracks.end(), std::greater<TrackRank>());

  // Snapshot size = max(N) + max(X) where X = max tracks per publisher
  // This ensures any pub-subscriber can find N non-self tracks even if
  // all their X tracks are at the top of the ranking
  uint8_t maxX = maxTracksPerPublisher_.load(std::memory_order_relaxed);
  size_t snapshotSize = std::min(
      allTracks.size(),
      static_cast<size_t>(currentMaxN) + static_cast<size_t>(maxX)
  );

  auto newSnapshot = std::make_shared<Snapshot>();
  newSnapshot->reserve(snapshotSize);

  for (size_t i = 0; i < snapshotSize; ++i) {
    newSnapshot->push_back(std::move(allTracks[i]));
  }

  std::atomic_store(&snapshot_, newSnapshot);
  snapshotVersion_.fetch_add(1, std::memory_order_relaxed);
}

void SimpleTopNTracker::updateMaxN() {
  uint8_t newMaxN = 0;
  for (const auto& [session, info] : sessions_) {
    newMaxN = std::max(newMaxN, static_cast<uint8_t>(info.maxSelected));
  }
  maxN_.store(newMaxN, std::memory_order_relaxed);

  if (newMaxN > 0) {
    rebuildSnapshot();
  }
}

void SimpleTopNTracker::updateMaxTracksPerPublisher() {
  uint8_t maxX = 0;
  for (const auto& [pub, count] : publisherTrackCount_) {
    maxX = std::max(maxX, static_cast<uint8_t>(std::min(count, static_cast<size_t>(255))));
  }
  maxTracksPerPublisher_.store(maxX, std::memory_order_relaxed);
}

uint64_t SimpleTopNTracker::findRankInSnapshot(
    const moxygen::FullTrackName& ftn,
    const Snapshot& snapshot
) const {
  for (size_t i = 0; i < snapshot.size(); ++i) {
    if (snapshot[i].ftn == ftn) {
      return i;
    }
  }
  return UINT64_MAX;
}

uint8_t SimpleTopNTracker::computeLastSelfPosition(
    const std::shared_ptr<moxygen::MoQSession>& session,
    const Snapshot& snapshot
) const {
  uint8_t lastPos = 0;
  auto* sessionRaw = session.get();

  for (size_t i = 0; i < snapshot.size(); ++i) {
    if (snapshot[i].publisherRaw == sessionRaw) {
      lastPos = static_cast<uint8_t>(std::min(i, static_cast<size_t>(255)));
    }
  }

  return lastPos;
}

// ----------------------------------------------------------------------------
// SimpleTopNRanking - handles per-session selection with self-exclusion
// ----------------------------------------------------------------------------

SimpleTopNRanking::SimpleTopNRanking(
    uint64_t propertyType,
    uint64_t /* maxDeselected */,
    std::chrono::milliseconds idleTimeout,
    std::chrono::milliseconds sweepThrottle,
    GetLastActivityFn getLastActivity,
    BatchSelectCallback onBatchSelected,
    SelectCallback onSelected,
    EvictCallback onEvicted
)
    : tracker_(
          propertyType,
          idleTimeout,
          sweepThrottle,
          std::move(getLastActivity),
          [this](auto old, auto newSnap, auto removed) {
            onSnapshotChanged(old, newSnap, removed);
          }
      ),
      onBatchSelected_(std::move(onBatchSelected)), onSelected_(std::move(onSelected)),
      onEvicted_(std::move(onEvicted)) {}

void SimpleTopNRanking::registerTrack(
    const moxygen::FullTrackName& ftn,
    std::optional<uint64_t> initialPropertyValue,
    std::shared_ptr<moxygen::MoQSession> publisher
) {
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    // If the publisher has a session state and was classified as pure subscriber,
    // promote it to publisher and remove from pure subscriber group.
    auto stateIt = sessionStates_.find(publisher);
    if (stateIt != sessionStates_.end() && !stateIt->second.isPublisher) {
      stateIt->second.isPublisher = true;
      uint64_t n = stateIt->second.maxSelected;
      auto groupIt = pureSubscriberGroups_.find(n);
      if (groupIt != pureSubscriberGroups_.end()) {
        auto& g = groupIt->second;
        for (size_t i = 0; i < g.sessions.size(); ++i) {
          if (g.sessions[i] == publisher) {
            g.sessions.erase(g.sessions.begin() + i);
            g.forwards.erase(g.forwards.begin() + i);
            break;
          }
        }
        if (g.sessions.empty()) {
          pureSubscriberGroups_.erase(groupIt);
        }
      }
    }
  }
  tracker_.registerTrack(ftn, initialPropertyValue, std::move(publisher));
}

void SimpleTopNRanking::updateSortValue(const moxygen::FullTrackName& ftn, uint64_t value) {
  tracker_.updateSortValue(ftn, value);
  tracker_.flush();
}

void SimpleTopNRanking::removeTrack(const moxygen::FullTrackName& ftn) {
  tracker_.removeTrack(ftn);
}

void SimpleTopNRanking::addSessionToTopNGroup(
    uint64_t maxSelected,
    std::shared_ptr<moxygen::MoQSession> session,
    bool forward
) {
  bool isPublisher = false;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);

    // Determine if this session publishes any tracks.
    // Check trackIndex_ directly — snapshot may be empty when maxN==0.
    isPublisher = tracker_.isSessionPublisher(session);

    if (!isPublisher) {
      // Pure subscriber — add to grouped pool (compute-once, apply-to-all)
      auto& group = pureSubscriberGroups_[maxSelected];
      group.sessions.push_back(session);
      group.forwards.push_back(forward);
    }

    sessionStates_[session] = SessionSelectionState{
        .maxSelected = maxSelected,
        .forward = forward,
        .selectedTracks = {},
        .cachedLastSelfPosition = 0,
        .cachedSnapshotVersion = 0,
        .isPublisher = isPublisher};

    auto& group = topNGroups_[maxSelected];
    group.maxSelected = maxSelected;
    group.sessions[session] = SubscriberFilter{
        .trackFilterN = static_cast<uint8_t>(maxSelected), .forward = forward};
  }

  tracker_.addSession(maxSelected, session, forward);
}

void SimpleTopNRanking::updateSessionForward(
    uint64_t maxSelected,
    const std::shared_ptr<moxygen::MoQSession>& session,
    bool forward
) {
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    auto it = sessionStates_.find(session);
    if (it != sessionStates_.end()) {
      it->second.forward = forward;
    }

    auto groupIt = topNGroups_.find(maxSelected);
    if (groupIt != topNGroups_.end()) {
      auto sessionIt = groupIt->second.sessions.find(session);
      if (sessionIt != groupIt->second.sessions.end()) {
        sessionIt->second.forward = forward;
      }
    }
  }

  tracker_.updateSessionForward(maxSelected, session, forward);
}

void SimpleTopNRanking::removeSessionFromTopNGroup(
    uint64_t maxSelected,
    const std::shared_ptr<moxygen::MoQSession>& session
) {
  {
    std::lock_guard<std::mutex> lock(stateMutex_);

    auto stateIt = sessionStates_.find(session);
    bool wasPublisher = stateIt != sessionStates_.end() && stateIt->second.isPublisher;
    sessionStates_.erase(stateIt);

    if (!wasPublisher) {
      // Remove from pure subscriber group
      auto groupIt = pureSubscriberGroups_.find(maxSelected);
      if (groupIt != pureSubscriberGroups_.end()) {
        auto& g = groupIt->second;
        for (size_t i = 0; i < g.sessions.size(); ++i) {
          if (g.sessions[i] == session) {
            g.sessions.erase(g.sessions.begin() + i);
            g.forwards.erase(g.forwards.begin() + i);
            break;
          }
        }
        if (g.sessions.empty()) {
          pureSubscriberGroups_.erase(groupIt);
        }
      }
    }

    auto it = topNGroups_.find(maxSelected);
    if (it != topNGroups_.end()) {
      it->second.sessions.erase(session);
      if (it->second.sessions.empty()) {
        topNGroups_.erase(it);
      }
    }
  }

  tracker_.removeSession(maxSelected, session);
}

const SimpleTopNRanking::TopNGroupCompat* SimpleTopNRanking::getTopNGroup(uint64_t maxSelected
) const {
  auto it = topNGroups_.find(maxSelected);
  return it != topNGroups_.end() ? &it->second : nullptr;
}

bool SimpleTopNRanking::topNBoundaryChanged(
    const Snapshot& oldSnap,
    const Snapshot& newSnap,
    size_t boundarySize
) {
  size_t oldSize = std::min(boundarySize, oldSnap.size());
  size_t newSize = std::min(boundarySize, newSnap.size());

  if (oldSize != newSize) {
    return true;
  }

  if (oldSize == 0) {
    return false;
  }

  // Positional comparison: if every position is identical, no subscriber's
  // selection can have changed. Any positional difference is conservatively
  // treated as a boundary change since internal reorders CAN affect
  // subscribers with N < boundarySize.
  for (size_t i = 0; i < oldSize; ++i) {
    if (oldSnap[i].ftn != newSnap[i].ftn) {
      return true;
    }
  }
  return false;
}

void SimpleTopNRanking::onSnapshotChanged(
    std::shared_ptr<const Snapshot> oldSnapshot,
    std::shared_ptr<const Snapshot> newSnapshot,
    std::optional<moxygen::FullTrackName> removedTrack
) {
  std::lock_guard<std::mutex> lock(stateMutex_);

  // Optimization 2: Early-exit when top-N boundary didn't change.
  // Note: oldSnapshot == newSnapshot (same pointer) signals initial selection for
  // a newly added session — never skip that case.
  bool isInitialSelection = (oldSnapshot.get() == newSnapshot.get());

  // Determine if snapshot membership changed (tracks added/removed) vs just reordering.
  // If sizes are equal and no track was removed, only ordering changed.
  bool snapshotMembershipChanged = isInitialSelection || !oldSnapshot || !newSnapshot ||
      (oldSnapshot->size() != newSnapshot->size()) || removedTrack.has_value();

  // Optimization 1: Pure subscribers grouped by N — compute once per group.
  for (auto& [n, group] : pureSubscriberGroups_) {
    // When N >= snapshot size, selection is ALL tracks in snapshot.
    // If membership didn't change, selection can't change — skip entirely.
    if (!snapshotMembershipChanged && n >= newSnapshot->size()) {
      continue;
    }

    // For N < snapshot size, check if the top-N positions changed
    if (!snapshotMembershipChanged) {
      if (!topNBoundaryChanged(*oldSnapshot, *newSnapshot, n)) {
        continue;
      }
    }

    size_t effectiveN = std::min(n, newSnapshot->size());

    // In-place delta: find evictions (in old set but not in new top-N)
    std::vector<moxygen::FullTrackName> evictions;
    for (const auto& ftn : group.selectedTracks) {
      bool inTopN = false;
      for (size_t i = 0; i < effectiveN; ++i) {
        if ((*newSnapshot)[i].ftn == ftn) { inTopN = true; break; }
      }
      if (!inTopN) {
        evictions.push_back(ftn);
      }
    }

    // In-place delta: find selections (in new top-N but not in old set)
    std::vector<moxygen::FullTrackName> selections;
    for (size_t i = 0; i < effectiveN; ++i) {
      if (group.selectedTracks.find((*newSnapshot)[i].ftn) == group.selectedTracks.end()) {
        selections.push_back((*newSnapshot)[i].ftn);
      }
    }

    // Apply evictions to all sessions
    for (const auto& ftn : evictions) {
      group.selectedTracks.erase(ftn);
      for (size_t i = 0; i < group.sessions.size(); ++i) {
        if (onEvicted_) {
          onEvicted_(ftn, group.sessions[i]);
        }
      }
    }

    // Apply selections to all sessions
    for (const auto& ftn : selections) {
      group.selectedTracks.insert(ftn);
      for (size_t i = 0; i < group.sessions.size(); ++i) {
        if (onSelected_) {
          onSelected_(ftn, group.sessions[i], group.forwards[i]);
        }
      }
    }

    // For initial selection (new session joined), send all existing selections
    // to the newest session that wasn't covered by the delta above.
    if (isInitialSelection && group.sessions.size() > 1) {
      size_t newIdx = group.sessions.size() - 1;
      for (const auto& ftn : group.selectedTracks) {
        // Skip tracks that were just added in the delta — already sent to all
        bool wasJustAdded = false;
        for (const auto& sel : selections) {
          if (sel == ftn) { wasJustAdded = true; break; }
        }
        if (!wasJustAdded && onSelected_) {
          onSelected_(ftn, group.sessions[newIdx], group.forwards[newIdx]);
        }
      }
    }
  }

  // Optimization 3: Only recompute pub-subscribers individually
  for (auto& [session, state] : sessionStates_) {
    if (!state.isPublisher) {
      continue;  // Already handled by group above
    }

    // Pub-subscriber with N >= non-self track count: selection is all non-self tracks.
    // If membership didn't change, their selection can't change.
    if (!snapshotMembershipChanged && state.maxSelected >= newSnapshot->size()) {
      continue;
    }

    // Boundary check for pub-subscribers: if the top-(N + selfTrackCount) positions
    // are unchanged, this subscriber's selection with self-exclusion can't change.
    // cachedLastSelfPosition is the furthest position of any self-track in the snapshot.
    if (!snapshotMembershipChanged && oldSnapshot && newSnapshot) {
      size_t boundarySize = state.maxSelected + state.cachedLastSelfPosition + 1;
      if (!topNBoundaryChanged(*oldSnapshot, *newSnapshot, boundarySize)) {
        continue;
      }
    }

    recomputeSessionSelection(session, state, *newSnapshot, removedTrack);
  }
}

void SimpleTopNRanking::recomputeSessionSelection(
    const std::shared_ptr<moxygen::MoQSession>& session,
    SessionSelectionState& state,
    const Snapshot& snapshot,
    std::optional<moxygen::FullTrackName> removedTrack
) {
  // Update cached self-position if snapshot changed
  uint64_t currentVersion = tracker_.snapshotVersion();
  if (state.cachedSnapshotVersion != currentVersion) {
    state.cachedLastSelfPosition = tracker_.computeLastSelfPosition(session, snapshot);
    state.cachedSnapshotVersion = currentVersion;
  }

  // Compute new selection with self-exclusion
  auto newSelection = tracker_.computeTopNForSession(session, state.maxSelected, snapshot);

  // In-place delta: find evictions (in old set but not in new selection)
  std::vector<moxygen::FullTrackName> evictions;
  for (const auto& ftn : state.selectedTracks) {
    bool inNew = false;
    for (const auto& sel : newSelection) {
      if (sel == ftn) { inNew = true; break; }
    }
    if (!inNew) {
      evictions.push_back(ftn);
    }
  }

  // In-place delta: find selections (in new but not in old set)
  std::vector<moxygen::FullTrackName> selections;
  for (const auto& ftn : newSelection) {
    if (state.selectedTracks.find(ftn) == state.selectedTracks.end()) {
      selections.push_back(ftn);
    }
  }

  // Apply evictions
  for (const auto& ftn : evictions) {
    state.selectedTracks.erase(ftn);
    TopNEventLog::instance().logTopNEvicted(ftn, session.get());
    if (onEvicted_) {
      onEvicted_(ftn, session);
    }
  }

  // Apply selections
  for (const auto& ftn : selections) {
    state.selectedTracks.insert(ftn);
    TopNEventLog::instance().logTopNSelected(ftn, session.get());
    if (onSelected_) {
      onSelected_(ftn, session, state.forward);
    }
  }
}

} // namespace openmoq::moqx
