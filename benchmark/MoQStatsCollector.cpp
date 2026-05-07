#include <folly/Benchmark.h>
#include "stats/MoQStatsCollector.h"

#include <array>

namespace {

using openmoq::moqx::stats::MoQStatsCollector;
using openmoq::moqx::stats::StatsRegistry;

// onSubscribeSuccess covers both the success and error code paths — the
// implementation is symmetric so a separate OnSubscribeError benchmark didn't
// add signal (per review feedback).
BENCHMARK(BM_StatsCollector_OnSubscribeSuccess, iters) {
  folly::BenchmarkSuspender susp;
  auto registry = std::make_shared<StatsRegistry>();
  auto collector = MoQStatsCollector::create_moq_stats_collector(registry);
  auto pubCb = collector->publisherCallback();
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    pubCb->onSubscribeSuccess();
  }
}

// Cycle through latency values that hit different histogram buckets so the
// bucket-search path is exercised under realistic mixed load (rather than
// pinning to a single bucket on every call).
BENCHMARK(BM_StatsCollector_RecordLatency, iters) {
  folly::BenchmarkSuspender susp;
  auto registry = std::make_shared<StatsRegistry>();
  auto collector = MoQStatsCollector::create_moq_stats_collector(registry);
  auto subCb = collector->subscriberCallback();
  static constexpr std::array<uint64_t, 8> kLatencies = {
      10, 75, 200, 500, 1500, 5000, 25000, 999999};
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    subCb->recordSubscribeLatency(kLatencies[i % kLatencies.size()]);
  }
}

// Worker-thread-hot path: aggregateAsync calls snapshot() to assemble the
// global view. Per review feedback, this is the one MoQStatsCollector benchmark
// that maps to a real-world load case worth tracking over time.
BENCHMARK(BM_StatsCollector_Snapshot, iters) {
  folly::BenchmarkSuspender susp;
  auto registry = std::make_shared<StatsRegistry>();
  auto collector = MoQStatsCollector::create_moq_stats_collector(registry);
  auto pubCb = collector->publisherCallback();
  for (int j = 0; j < 1000; ++j) {
    pubCb->onSubscribeSuccess();
    pubCb->onFetchSuccess();
  }
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    auto snap = collector->snapshot();
    folly::doNotOptimizeAway(snap);
  }
}

BENCHMARK(BM_StatsCollector_SessionLifecycle, iters) {
  folly::BenchmarkSuspender susp;
  auto registry = std::make_shared<StatsRegistry>();
  auto collector = MoQStatsCollector::create_moq_stats_collector(registry);
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    collector->onSessionStart();
    collector->onSessionEnd();
  }
}

} // namespace
