#include <folly/Benchmark.h>
#include "stats/StatsRegistry.h"

namespace {

using openmoq::moqx::stats::StatsSnapshot;

// Aggregation operator+= is the worker-thread-hot path used by
// StatsRegistry::aggregateAsync to fold per-session deltas into a global
// snapshot. This is the only StatsSnapshot operation that's worth measuring
// here; format-to-Prometheus is covered by the dedicated PrometheusFormat
// benchmark, and tiny enum-to-index lookups are not benchmark-worthy.
BENCHMARK(BM_StatsSnapshot_Accumulate, iters) {
  folly::BenchmarkSuspender susp;
  StatsSnapshot base{};
  StatsSnapshot delta{};
  delta.pubSubscribeSuccess = 1;
  delta.quicPacketsReceived = 100;
  delta.moqActiveSessions = 5;
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    base += delta;
    folly::doNotOptimizeAway(base);
  }
}

} // namespace
