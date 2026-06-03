/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "relay/PropertyRanking.h"
#include "relay/SimpleTopNTracker.h"

#include <memory>
#include <variant>

namespace openmoq::moqx {

/**
 * TopNRankingMode - Toggle for selecting ranking implementation.
 */
enum class TopNRankingMode {
  // Original implementation with push-based notifications, per-N TopNGroups,
  // waterline-based self-exclusion, and deselected queue.
  Complex,

  // Simplified lock-free implementation with pull-based evaluation,
  // single global snapshot, and transport-level self-exclusion.
  Simple
};

/**
 * ITopNRanking - Common interface for both ranking implementations.
 * Enables runtime switching between Complex (PropertyRanking) and Simple (SimpleTopNRanking).
 */
class ITopNRanking {
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

  using GetLastActivityFn =
      std::function<std::chrono::steady_clock::time_point(const moxygen::FullTrackName&)>;

  virtual ~ITopNRanking() = default;

  virtual uint64_t propertyType() const = 0;

  virtual void registerTrack(
      const moxygen::FullTrackName& ftn,
      std::optional<uint64_t> initialPropertyValue,
      std::shared_ptr<moxygen::MoQSession> publisher
  ) = 0;

  virtual void updateSortValue(const moxygen::FullTrackName& ftn, uint64_t value) = 0;

  virtual void removeTrack(const moxygen::FullTrackName& ftn) = 0;

  virtual void addSessionToTopNGroup(
      uint64_t maxSelected,
      std::shared_ptr<moxygen::MoQSession> session,
      bool forward
  ) = 0;

  virtual void updateSessionForward(
      uint64_t maxSelected,
      const std::shared_ptr<moxygen::MoQSession>& session,
      bool forward
  ) = 0;

  virtual void removeSessionFromTopNGroup(
      uint64_t maxSelected,
      const std::shared_ptr<moxygen::MoQSession>& session
  ) = 0;

  virtual void sweepIdle() = 0;

  /**
   * Flush coalesced value updates — rebuild snapshot and fire callbacks.
   * Call after updateSortValue; respects the configured flush interval.
   * Default no-op for implementations that process updates eagerly.
   */
  virtual void flush() {}

  /**
   * Set minimum interval between flushes for coalescing value updates.
   * Default 0 = no coalescing (every flush rebuilds immediately).
   */
  virtual void setFlushInterval(std::chrono::milliseconds /*interval*/) {}

  virtual bool empty() const = 0;
  virtual size_t numTracks() const = 0;
};

/**
 * PropertyRankingAdapter - Wraps PropertyRanking to implement ITopNRanking.
 */
class PropertyRankingAdapter : public ITopNRanking {
public:
  PropertyRankingAdapter(
      uint64_t propertyType,
      uint64_t maxDeselected,
      std::chrono::milliseconds idleTimeout,
      std::chrono::milliseconds sweepThrottle,
      GetLastActivityFn getLastActivity,
      BatchSelectCallback onBatchSelected,
      SelectCallback onSelected,
      EvictCallback onEvicted
  )
      : ranking_(
            propertyType,
            maxDeselected,
            idleTimeout,
            sweepThrottle,
            std::move(getLastActivity),
            std::move(onBatchSelected),
            std::move(onSelected),
            std::move(onEvicted)
        ) {}

  uint64_t propertyType() const override { return ranking_.propertyType(); }

  void registerTrack(
      const moxygen::FullTrackName& ftn,
      std::optional<uint64_t> initialPropertyValue,
      std::shared_ptr<moxygen::MoQSession> publisher
  ) override {
    ranking_.registerTrack(ftn, initialPropertyValue, std::move(publisher));
  }

  void updateSortValue(const moxygen::FullTrackName& ftn, uint64_t value) override {
    ranking_.updateSortValue(ftn, value);
  }

  void removeTrack(const moxygen::FullTrackName& ftn) override { ranking_.removeTrack(ftn); }

  void addSessionToTopNGroup(
      uint64_t maxSelected,
      std::shared_ptr<moxygen::MoQSession> session,
      bool forward
  ) override {
    ranking_.addSessionToTopNGroup(maxSelected, std::move(session), forward);
  }

  void updateSessionForward(
      uint64_t maxSelected,
      const std::shared_ptr<moxygen::MoQSession>& session,
      bool forward
  ) override {
    ranking_.updateSessionForward(maxSelected, session, forward);
  }

  void removeSessionFromTopNGroup(
      uint64_t maxSelected,
      const std::shared_ptr<moxygen::MoQSession>& session
  ) override {
    ranking_.removeSessionFromTopNGroup(maxSelected, session);
  }

  void sweepIdle() override { ranking_.sweepIdle(); }

  bool empty() const override { return ranking_.empty(); }
  size_t numTracks() const override { return ranking_.numTracks(); }

  // Direct access to underlying implementation for tests
  PropertyRanking& underlying() { return ranking_; }
  const PropertyRanking& underlying() const { return ranking_; }

private:
  PropertyRanking ranking_;
};

/**
 * SimpleTopNRankingAdapter - Wraps SimpleTopNRanking to implement ITopNRanking.
 */
class SimpleTopNRankingAdapter : public ITopNRanking {
public:
  SimpleTopNRankingAdapter(
      uint64_t propertyType,
      uint64_t maxDeselected,
      std::chrono::milliseconds idleTimeout,
      std::chrono::milliseconds sweepThrottle,
      GetLastActivityFn getLastActivity,
      BatchSelectCallback onBatchSelected,
      SelectCallback onSelected,
      EvictCallback onEvicted
  )
      : ranking_(
            propertyType,
            maxDeselected,
            idleTimeout,
            sweepThrottle,
            std::move(getLastActivity),
            std::move(onBatchSelected),
            std::move(onSelected),
            std::move(onEvicted)
        ) {}

  uint64_t propertyType() const override { return ranking_.propertyType(); }

  void registerTrack(
      const moxygen::FullTrackName& ftn,
      std::optional<uint64_t> initialPropertyValue,
      std::shared_ptr<moxygen::MoQSession> publisher
  ) override {
    ranking_.registerTrack(ftn, initialPropertyValue, std::move(publisher));
  }

  void updateSortValue(const moxygen::FullTrackName& ftn, uint64_t value) override {
    ranking_.updateSortValue(ftn, value);
  }

  void removeTrack(const moxygen::FullTrackName& ftn) override { ranking_.removeTrack(ftn); }

  void addSessionToTopNGroup(
      uint64_t maxSelected,
      std::shared_ptr<moxygen::MoQSession> session,
      bool forward
  ) override {
    ranking_.addSessionToTopNGroup(maxSelected, std::move(session), forward);
  }

  void updateSessionForward(
      uint64_t maxSelected,
      const std::shared_ptr<moxygen::MoQSession>& session,
      bool forward
  ) override {
    ranking_.updateSessionForward(maxSelected, session, forward);
  }

  void removeSessionFromTopNGroup(
      uint64_t maxSelected,
      const std::shared_ptr<moxygen::MoQSession>& session
  ) override {
    ranking_.removeSessionFromTopNGroup(maxSelected, session);
  }

  void sweepIdle() override { ranking_.sweepIdle(); }

  void flush() override { ranking_.flush(); }

  void setFlushInterval(std::chrono::milliseconds interval) override {
    ranking_.setFlushInterval(interval);
  }

  bool empty() const override { return ranking_.empty(); }
  size_t numTracks() const override { return ranking_.numTracks(); }

  // Direct access to underlying implementation for tests
  SimpleTopNRanking& underlying() { return ranking_; }
  const SimpleTopNRanking& underlying() const { return ranking_; }

private:
  SimpleTopNRanking ranking_;
};

/**
 * TopNRankingFactory - Creates ranking instances based on mode.
 */
class TopNRankingFactory {
public:
  static std::shared_ptr<ITopNRanking> create(
      TopNRankingMode mode,
      uint64_t propertyType,
      uint64_t maxDeselected,
      std::chrono::milliseconds idleTimeout,
      std::chrono::milliseconds sweepThrottle,
      ITopNRanking::GetLastActivityFn getLastActivity,
      ITopNRanking::BatchSelectCallback onBatchSelected,
      ITopNRanking::SelectCallback onSelected,
      ITopNRanking::EvictCallback onEvicted
  ) {
    switch (mode) {
      case TopNRankingMode::Simple:
        return std::make_shared<SimpleTopNRankingAdapter>(
            propertyType,
            maxDeselected,
            idleTimeout,
            sweepThrottle,
            std::move(getLastActivity),
            std::move(onBatchSelected),
            std::move(onSelected),
            std::move(onEvicted)
        );

      case TopNRankingMode::Complex:
      default:
        return std::make_shared<PropertyRankingAdapter>(
            propertyType,
            maxDeselected,
            idleTimeout,
            sweepThrottle,
            std::move(getLastActivity),
            std::move(onBatchSelected),
            std::move(onSelected),
            std::move(onEvicted)
        );
    }
  }
};

} // namespace openmoq::moqx
