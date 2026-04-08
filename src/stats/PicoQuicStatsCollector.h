#pragma once

#include <memory>

#include <folly/Executor.h>
#include <folly/io/async/EventBase.h>
#include <moxygen/openmoq/transport/pico/PicoQuicStatsCallback.h>

#include "stats/StatsRegistry.h"

namespace openmoq::moqx::stats {

/**
 * Collects transport-layer QUIC statistics from a picoquic-based server.
 *
 * Implements moxygen::PicoQuicStatsCallback — register with
 * MoQPicoServerBase::setPicoQuicStatsCallback after constructing via create().
 *
 * Threading: All callback methods fire on the server's single EventBase;
 * counters need no synchronization. The EventBase is supplied at construction.
 */
class PicoQuicStatsCollector : public StatsCollectorBase, public moxygen::PicoQuicStatsCallback {
public:
  static std::shared_ptr<PicoQuicStatsCollector>
  create(std::shared_ptr<StatsRegistry> registry, folly::EventBase* evb);

  ~PicoQuicStatsCollector() override = default;

  // StatsCollectorBase
  StatsSnapshot snapshot() const override;
  folly::Executor* owningExecutor() const override;

  // moxygen::PicoQuicStatsCallback
  void onConnectionCreated() override;
  void onConnectionClosed() override;
  void onStreamCreated() override;
  void onStreamClosed() override;
  void onStreamReset() override;
  void onPathQualityDelta(const PathQualityDelta& d) override;

private:
  PicoQuicStatsCollector(folly::EventBase* evb);

  folly::EventBase* evb_;

  // Counters and gauges — all writes on evb_, no sync needed.
#define DEFINE_FIELD(type, name) type name##_{0};
  STATS_QUIC_COUNTER_FIELDS(DEFINE_FIELD)
  STATS_QUIC_GAUGE_FIELDS(DEFINE_FIELD)
#undef DEFINE_FIELD
};

} // namespace openmoq::moqx::stats
