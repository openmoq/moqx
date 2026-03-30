#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include <folly/Executor.h>
#include <folly/io/async/EventBase.h>
#include <quic/state/QuicTransportStatsCallback.h>

#include "moqx/stats/StatsRegistry.h"

namespace openmoq::moqx::stats {

/**
 * Collects transport-layer QUIC statistics on a per-worker-thread basis.
 *
 * Ownership: Registry's shared_ptr is copied by aggregateAsync(), keeping the collector
 * alive during snapshot tasks even if mvfst destroys its Callback concurrently.
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
    std::unique_ptr<quic::QuicTransportStatsCallback> make() override;

  private:
    std::weak_ptr<StatsRegistry> registry_;
  };

  ~QuicStatsCollector() override;

  // Implement StatsCollectorBase
  StatsSnapshot snapshot() const override;

  folly::Executor* owningExecutor() const override;

private:
  // Callback is the quic::QuicTransportStatsCallback adapter owned by mvfst
  class Callback;

  // Private constructor: instantiated by Callback
  explicit QuicStatsCollector(std::weak_ptr<StatsRegistry> registry);

  std::atomic<folly::EventBase*> owningEvb_{nullptr};
  std::weak_ptr<StatsRegistry> registry_;

  // Counters (no sync needed; all writes on the QUIC IO EventBase).
#define DEFINE_FIELD(type, name) type name##_{0};
  STATS_QUIC_COUNTER_FIELDS(DEFINE_FIELD)
#undef DEFINE_FIELD
};

} // namespace openmoq::moqx::stats
