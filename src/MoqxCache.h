/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Originally from github.com/facebookexperimental/moxygen.
 * See deps/moxygen/LICENSE for the original license terms.
 *
 * Copyright (c) OpenMOQ contributors.
 */

#pragma once

#include <chrono>
#include <folly/container/F14Set.h>
#include <folly/coro/Baton.h>
#include <folly/coro/Task.h>
#include <functional>
#include <moxygen/MoQConsumers.h>
#include <moxygen/MoQFramer.h>
#include <moxygen/Publisher.h>
#include <moxygen/util/LocationIntervalSet.h>

namespace openmoq::moqx {

// Default cache size limits
constexpr size_t kDefaultMaxCachedTracks = 100;
constexpr size_t kDefaultMaxCachedGroupsPerTrack = 3;
constexpr size_t kDefaultMaxCachedBytes = 10 * 1024 * 1024; // 10 MB
constexpr size_t kDefaultMinEvictionBytes = 100 * 1024;     // 100 KB

class MoqxCache {
public:
  using SteadyClock = std::chrono::steady_clock;
  using TimePoint = SteadyClock::time_point;

  explicit MoqxCache(
      size_t maxCachedTracks = kDefaultMaxCachedTracks,
      size_t maxCachedGroupsPerTrack = kDefaultMaxCachedGroupsPerTrack,
      size_t maxCachedBytes = kDefaultMaxCachedBytes,
      size_t minEvictionBytes = kDefaultMinEvictionBytes
  )
      : maxCachedTracks_(maxCachedTracks), maxCachedGroupsPerTrack_(maxCachedGroupsPerTrack),
        maxCachedBytes_(maxCachedBytes), minEvictionBytes_(minEvictionBytes),
        clock_([]() { return SteadyClock::now(); }) {}

  // Returns a filter for a subscribe that writes objects to the cache and
  // passes to the next consumer
  std::shared_ptr<moxygen::TrackConsumer> getSubscribeWriteback(
      const moxygen::FullTrackName& ftn,
      std::shared_ptr<moxygen::TrackConsumer> consumer
  );

  // Returns a TrackConsumer suitable for use as a passive forwarder subscriber.
  // Objects delivered to it are written to the cache; nothing is forwarded
  // downstream (the inner consumer is a NullTrackConsumer).
  std::shared_ptr<moxygen::TrackConsumer> makePassiveConsumer(const moxygen::FullTrackName& ftn);

  // Serves objects from the cache to the consumer.  If objects in the range are
  // not in cache, issue one-or-more FETCH'es upstream.  Objects fetched from
  // upstream are written back to the cache and passed to the consumer.
  //
  // MoqxCache internally coalesces multiple concurrent upstream requests for the
  // same object.
  folly::coro::Task<moxygen::Publisher::FetchResult> fetch(
      moxygen::Fetch fetch,
      std::shared_ptr<moxygen::FetchConsumer> consumer,
      std::shared_ptr<moxygen::Publisher> upstream
  );

  void clear() {
    for (auto& [ftn, track] : cache_) {
      track->evicted = true;
    }
    cache_.clear();
    trackLRU_.clear();
    totalCachedBytes_ = 0;
  }

  // Force-evicts a specific track unconditionally. Returns 1 if found, 0 if
  // not.
  size_t purge(const moxygen::FullTrackName& ftn) { return evictTrack(ftn); }

  // Force-evicts all tracks in the given namespace unconditionally.
  size_t purge(const moxygen::TrackNamespace& ns);

  // Force-evicts all cached tracks unconditionally.
  size_t purge();

  bool hasTrack(const moxygen::FullTrackName& ftn) const { return cache_.contains(ftn); }

  size_t size() const { return cache_.size(); }

  // Helper for testing - checks if object is cached
  bool hasCachedObject(const moxygen::FullTrackName& ftn, moxygen::AbsoluteLocation obj) {
    auto it = cache_.find(ftn);
    if (it == cache_.end()) {
      return false;
    }
    return getCachedObjectMaybe(*it->second, obj, now()) != nullptr;
  }

  // Setters for testing - update cache limits and evict if necessary
  void setMaxCachedTracks(size_t maxTracks) {
    maxCachedTracks_ = maxTracks;
    // Evict tracks if now over limit
    while (maxCachedTracks_ > 0 && cache_.size() > maxCachedTracks_ && !trackLRU_.empty()) {
      evictOldestTrackIfNeeded();
    }
  }

  void setMaxCachedBytes(size_t maxBytes) {
    maxCachedBytes_ = maxBytes;
    evictForByteLimitIfNeeded();
  }

  void setMinEvictionBytes(size_t minBytes) { minEvictionBytes_ = minBytes; }

  void setMaxCachedGroupsPerTrack(size_t maxGroups) {
    auto oldMax = maxCachedGroupsPerTrack_;
    maxCachedGroupsPerTrack_ = maxGroups;
    // Evict groups from all tracks if limit decreased
    if (maxGroups < oldMax) {
      for (auto& [ftn, track] : cache_) {
        evictOldestGroupsIfNeeded(*track);
      }
    }
  }

  // Sets the default max cache duration applied to all tracks that do not have
  // a per-track duration set via setMaxCacheDuration(). Pass std::nullopt to
  // remove the default (objects never expire by default).
  void setDefaultMaxCacheDuration(std::optional<std::chrono::milliseconds> duration) {
    defaultMaxCacheDuration_ = duration;
  }
  std::optional<std::chrono::milliseconds> getDefaultMaxCacheDuration() const {
    return defaultMaxCacheDuration_;
  }

  // Sets a hard upper bound on per-track cache durations. Any duration
  // extracted from publisher extensions that exceeds this cap is clamped to it.
  // Pass std::nullopt to remove the cap.
  void setMaxAllowedCacheDuration(std::optional<std::chrono::milliseconds> duration) {
    maxAllowedCacheDuration_ = duration;
  }
  std::optional<std::chrono::milliseconds> getMaxAllowedCacheDuration() const {
    return maxAllowedCacheDuration_;
  }

  void setTrackExtensions(const moxygen::FullTrackName& ftn, moxygen::Extensions extensions);

  TimePoint now() const { return clock_(); }

  void setClockForTesting(std::function<TimePoint()> clock) { clock_ = std::move(clock); }

  // Returns total payload bytes currently held in the cache.
  size_t totalCachedBytes() const { return totalCachedBytes_; }

  // Per-group stats for getTrackStats().
  struct GroupStats {
    uint64_t groupId;
    size_t objects;
  };

  // Per-track stats for getTrackStats().
  struct TrackStats {
    moxygen::FullTrackName name;
    bool endOfTrack;
    TimePoint lastWrite;
    std::vector<GroupStats> groups; // sorted by groupId ascending
  };

  // Returns a snapshot of all cached tracks and their group/object counts.
  // Groups within each track are returned in ascending groupId order.
  std::vector<TrackStats> getTrackStats() const;

  // Entry for single cached object
  struct CacheEntry {
    CacheEntry(
        uint64_t inSubgroup,
        moxygen::ObjectStatus inStatus,
        moxygen::Extensions inExtensions,
        moxygen::Payload inPayload,
        size_t inPayloadSize,
        bool inComplete,
        bool inForwardingPreferenceIsDatagram = false,
        TimePoint inCachedAt = TimePoint::min()
    )
        : subgroup(inSubgroup), status(inStatus), extensions(std::move(inExtensions)),
          payload(std::move(inPayload)), payloadSize(inPayloadSize), complete(inComplete),
          forwardingPreferenceIsDatagram(inForwardingPreferenceIsDatagram), cachedAt(inCachedAt) {}

    uint64_t subgroup{0};
    moxygen::ObjectStatus status;
    moxygen::Extensions extensions;
    moxygen::Payload payload;
    size_t payloadSize; // Cached byte count — avoids recomputing chain length
    bool complete{false};
    bool forwardingPreferenceIsDatagram{false};
    TimePoint cachedAt;
  };

private:
  class SubscribeWriteback;
  class SubgroupWriteback;
  class FetchWriteback;
  class FetchHandle;
  struct CacheTrack; // Forward declaration for CacheGroup::cacheObject

  // Entry for a group
  struct CacheGroup {
    folly::F14FastMap<uint64_t, std::unique_ptr<CacheEntry>> objects;
    // TODO: may be redundant now that FetchRangeIterator::skipGaps() uses gap
    // data to advance past group boundaries. Consider removing along with
    // findGroupEndMaybe.
    uint64_t maxCachedObject{0};
    bool endOfGroup{false};
    // Track seen Prior Group ID Gap value for validation
    // (all objects in a group must have the same gap value)
    std::optional<uint64_t> seenPriorGroupIdGap;
    // Total payload bytes across all objects in this group
    size_t totalBytes{0};
    // Per-track LRU iterator - present if group is evictable (not in active
    // SubgroupWriteback). Used by evictOldestGroupsIfNeeded().
    folly::Optional<std::list<uint64_t>::iterator> lruIter_;
    // Global LRU iterator - present if group is evictable.
    // Used by evictForByteLimitIfNeeded().
    folly::Optional<std::list<std::pair<moxygen::FullTrackName, uint64_t>>::iterator>
        globalLruIter_;

    folly::Expected<folly::Unit, moxygen::MoQPublishError> cacheObject(
        CacheTrack& track,
        uint64_t groupID,
        uint64_t subgroup,
        uint64_t objectID,
        moxygen::ObjectStatus status,
        const moxygen::Extensions& extensions,
        moxygen::Payload payload,
        bool complete,
        bool forwardingPreferenceIsDatagram,
        TimePoint now
    );
  };

  // Map from fetch range start to (end, writeback). Supports O(log n)
  // range queries to find if a position is being fetched upstream.
  struct FetchInProgressEntry {
    moxygen::AbsoluteLocation end;
    moxygen::AbsoluteLocation progress;
    FetchWriteback* writeback{};
  };
  using FetchesInProgressMap = std::map<moxygen::AbsoluteLocation, FetchInProgressEntry>;

  struct CacheTrack {
    folly::F14FastMap<uint64_t, std::shared_ptr<CacheGroup>> groups;
    moxygen::LocationIntervalSet gaps;
    moxygen::LocationIntervalSet cachedContent;
    size_t liveWritebackCount{0};
    bool endOfTrack{false};
    std::optional<moxygen::AbsoluteLocation> largestGroupAndObject;
    FetchesInProgressMap fetchesInProgress;
    // LRU iterator - present if track is evictable (not live, no active
    // fetches)
    folly::Optional<std::list<moxygen::FullTrackName>::iterator> lruIter_;
    // Group LRU list for this track
    std::list<uint64_t> groupLRU;
    // Optional max cache duration for this track
    std::optional<std::chrono::milliseconds> maxCacheDuration;
    // Track-level extensions to include in FetchOk
    moxygen::Extensions extensions;
    // Set by clear() — writebacks skip caching when true
    bool evicted{false};
    // Time of the most recent object write into this track
    TimePoint lastWrite{TimePoint::min()};

    folly::Expected<folly::Unit, moxygen::MoQPublishError>
    updateLargest(moxygen::AbsoluteLocation current, bool endOfTrack = false);
    CacheGroup& getOrCreateGroup(uint64_t groupID);
    // Same as getOrCreateGroup but, when creating, evicts old groups to honor
    // the per-track group limit and inserts the new group into the LRU.
    CacheGroup& getOrCreateGroupWithEviction(
        uint64_t groupID,
        MoqxCache& cache,
        const moxygen::FullTrackName& ftn
    );

    // Process Prior Group ID Gap and Prior Object ID Gap extensions
    // and mark the NonExistent groups/objects accordingly
    folly::Expected<folly::Unit, moxygen::MoQPublishError> processGapExtensions(
        uint64_t groupID,
        uint64_t objectID,
        const moxygen::Extensions& objectExtensions
    );

    // Find the unprocessed FetchWriteback covering a location, or nullptr
    FetchWriteback* findFetchInProgress(moxygen::AbsoluteLocation loc);

    // Returns true if track can be evicted (not live, no active fetches)
    bool canEvict() const { return liveWritebackCount == 0 && fetchesInProgress.empty(); }

    // Returns true if objects should be forwarded without caching.
    // Optimistic: nullopt maxCacheDuration means "unknown, cache it".
    bool shouldSkipCaching() const {
      return evicted || (maxCacheDuration && maxCacheDuration->count() == 0);
    }

    // Insert a known-non-existent range into both gaps and cachedContent.
    // The two sets must stay aligned: cachedContent doubles as the union
    // (real objects + known gaps) used by skipUncached().
    void insertGap(moxygen::AbsoluteLocation start, moxygen::AbsoluteLocation end) {
      gaps.insert(start, end);
      cachedContent.insert(start, end);
    }
  };

  // Group-order-aware iterator for traversing groups (objects within groups
  // always ascending). Automatically skips over gaps.
  class FetchRangeIterator {
  public:
    FetchRangeIterator(
        moxygen::AbsoluteLocation start,
        moxygen::AbsoluteLocation end,
        moxygen::GroupOrder order,
        std::shared_ptr<CacheTrack> track
    );

    moxygen::AbsoluteLocation end();
    void next();
    void skipGaps();     // Skip over any gaps at current position
    void skipUncached(); // Skip to next cached content position
    void advanceTo(const moxygen::AbsoluteLocation& loc);
    const moxygen::AbsoluteLocation& operator*() const;
    const moxygen::AbsoluteLocation* operator->() const;
    void invalidate();
    bool isValid() const;

    const moxygen::AbsoluteLocation minLocation;
    const moxygen::AbsoluteLocation maxLocation;
    const moxygen::GroupOrder order;
    std::shared_ptr<CacheTrack> track;

  private:
    void advanceOne(); // Single step without gap skipping
    moxygen::AbsoluteLocation current_;
    moxygen::AbsoluteLocation end_;
    bool isValid_ = true;
    mutable uint64_t cachedGroupId_{moxygen::kLocationMax.group};
    mutable std::shared_ptr<CacheGroup> cachedGroupPtr_{nullptr};
    mutable uint64_t cachedEndGroupId_{moxygen::kLocationMax.group};
    mutable std::shared_ptr<CacheGroup> cachedEndGroupPtr_{nullptr};
    std::optional<uint64_t> findGroupEndMaybe(
        uint64_t groupId,
        uint64_t& cachedGroupId_,
        std::shared_ptr<CacheGroup>& cachedGroupPtr_
    ) const;
    // Descending-mode helper: find the next cached position by jumping
    // to a lower group. Returns nullopt if no such position exists in
    // [minLocation, current_.group).
    std::optional<moxygen::AbsoluteLocation> findPrevGroupCachedPosition() const;
  };

  folly::
      F14FastMap<moxygen::FullTrackName, std::shared_ptr<CacheTrack>, moxygen::FullTrackName::hash>
          cache_;

  // LRU list of evictable tracks (oldest at back)
  std::list<moxygen::FullTrackName> trackLRU_;
  // Global LRU of evictable groups across all tracks (oldest at back).
  // Entry: {moxygen::FullTrackName, groupID}. Used by evictForByteLimitIfNeeded().
  std::list<std::pair<moxygen::FullTrackName, uint64_t>> globalGroupLRU_;

  // Cache size limits
  size_t maxCachedTracks_;
  size_t maxCachedGroupsPerTrack_;
  size_t maxCachedBytes_;
  size_t minEvictionBytes_;
  size_t totalCachedBytes_{0};

  // Default max cache duration applied to tracks without a per-track duration.
  // std::nullopt means objects do not expire by default.
  std::optional<std::chrono::milliseconds> defaultMaxCacheDuration_;

  // Hard cap on per-track durations extracted from publisher extensions.
  // std::nullopt means no cap is enforced.
  std::optional<std::chrono::milliseconds> maxAllowedCacheDuration_;

  // Injectable clock for testing
  std::function<TimePoint()> clock_;

  // Returns:
  // Returns valid CacheEntry* on cache hit, nullptr on miss.
  // Caller (fetchImpl) is responsible for gap-skipping via FetchRangeIterator.
  CacheEntry* getCachedObjectMaybe(CacheTrack& track, moxygen::AbsoluteLocation obj, TimePoint now);

  folly::coro::Task<moxygen::Publisher::FetchResult> fetchImpl(
      std::shared_ptr<FetchHandle> fetchHandle,
      moxygen::Fetch fetch,
      std::shared_ptr<CacheTrack> track,
      std::shared_ptr<moxygen::FetchConsumer> consumer,
      std::shared_ptr<moxygen::Publisher> upstream
  );

  folly::coro::Task<moxygen::Publisher::FetchResult> fetchUpstream(
      std::shared_ptr<MoqxCache::FetchHandle> fetchHandle,
      const moxygen::AbsoluteLocation& fetchStart,
      const moxygen::AbsoluteLocation& fetchEnd,
      bool lastObject,
      moxygen::Fetch fetch,
      std::shared_ptr<CacheTrack> track,
      std::shared_ptr<moxygen::FetchConsumer> consumer,
      std::shared_ptr<moxygen::Publisher> upstream
  );

  folly::coro::Task<folly::Expected<folly::Unit, moxygen::FetchError>>
  handleBlocked(std::shared_ptr<moxygen::FetchConsumer> consumer, const moxygen::Fetch& fetch);

  void setMaxCacheDuration(const moxygen::FullTrackName& ftn, std::chrono::milliseconds duration);
  void clearMaxCacheDuration(const moxygen::FullTrackName& ftn);

  // Returns publisher duration (clamped to maxAllowedCacheDuration_), or
  // defaultMaxCacheDuration_, or nullopt if neither is set.
  std::optional<std::chrono::milliseconds>
  getEffectiveCacheDuration(const moxygen::Extensions& extensions) const;

  // Track LRU management helpers
  void addTrackToLRU(const moxygen::FullTrackName& ftn, CacheTrack& track);
  void removeTrackFromLRU(CacheTrack& track);
  void onTrackBecameEvictable(const moxygen::FullTrackName& ftn);

  // Group LRU management helpers
  void addGroupToLRU(
      const moxygen::FullTrackName& ftn,
      uint64_t groupID,
      CacheGroup& group,
      CacheTrack& track
  );
  void removeGroupFromLRU(CacheGroup& group, CacheTrack& track);

  // Eviction methods
  bool evictOldestTrackIfNeeded();
  size_t evictTrack(const moxygen::FullTrackName& ftn);
  void evictOldestGroupsIfNeeded(CacheTrack& track);
  void evictGroup(CacheTrack& track, uint64_t groupID);
  bool evictForByteLimitIfNeeded();

  // Wraps cacheObject() + byte accounting + eviction check + lastWrite update
  folly::Expected<folly::Unit, moxygen::MoQPublishError> cacheObjectAndUpdateBytes(
      CacheGroup& group,
      CacheTrack& track,
      uint64_t groupID,
      uint64_t subgroup,
      uint64_t objectID,
      moxygen::ObjectStatus status,
      const moxygen::Extensions& extensions,
      moxygen::Payload payload,
      bool complete,
      bool forwardingPreferenceIsDatagram,
      TimePoint now
  );
};

} // namespace openmoq::moqx
