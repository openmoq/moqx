/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include <folly/Executor.h>
#include <folly/io/async/EventBase.h>
#include <quic/state/QuicTransportStatsCallback.h>

#include "stats/StatsRegistry.h"

namespace openmoq::moqx::stats {

/**
 * Collects transport-layer QUIC statistics on a per-worker-thread basis.
 *
 * Ownership: The Callback holds a shared_ptr to the collector it creates.
 *
 * Threading: All callback methods fire on the QUIC worker's serialized EventBase thread;
 * counters need no synchronization. Registration occurs after executor capture, so it's always
 * valid.
 */
class QuicStatsCollector : public StatsCollectorBase {
public:
  // Factory passed to mvfst to create a Callback
  class Factory : public quic::QuicTransportStatsCallbackFactory {
  public:
    explicit Factory(std::shared_ptr<StatsRegistry> registry);
    // Called once per QUIC worker thread to create a per-worker Callback instance.
    std::unique_ptr<quic::QuicTransportStatsCallback> make() override;

  private:
    std::weak_ptr<StatsRegistry> registry_;
  };

  ~QuicStatsCollector() override = default;

  // Implement StatsCollectorBase
  StatsSnapshot snapshot() const override;

  folly::Executor* owningExecutor() const override;

private:
  // Callback is the quic::QuicTransportStatsCallback adapter owned by mvfst
  class Callback;

  // Private constructor: instantiated by Callback
  QuicStatsCollector() = default;

  std::atomic<folly::EventBase*> owningEvb_{nullptr};

  // Counters and gauges (no sync needed; all writes on the QUIC IO EventBase).
#define DEFINE_FIELD(type, name) type name##_{0};
  STATS_QUIC_COUNTER_FIELDS(DEFINE_FIELD)
  STATS_QUIC_GAUGE_FIELDS(DEFINE_FIELD)
#undef DEFINE_FIELD
};

} // namespace openmoq::moqx::stats
