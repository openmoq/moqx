#include <folly/Benchmark.h>
#include <moqx/stats/MoQStatsCollector.h>

namespace {

using openmoq::moqx::stats::MoQStatsCollector;
using openmoq::moqx::stats::StatsRegistry;

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

BENCHMARK(BM_StatsCollector_OnSubscribeError, iters) {
  folly::BenchmarkSuspender susp;
  auto registry = std::make_shared<StatsRegistry>();
  auto collector = MoQStatsCollector::create_moq_stats_collector(registry);
  auto pubCb = collector->publisherCallback();
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    pubCb->onSubscribeError(moxygen::RequestErrorCode::INTERNAL_ERROR);
  }
}

BENCHMARK(BM_StatsCollector_RecordLatency, iters) {
  folly::BenchmarkSuspender susp;
  auto registry = std::make_shared<StatsRegistry>();
  auto collector = MoQStatsCollector::create_moq_stats_collector(registry);
  auto subCb = collector->subscriberCallback();
  uint64_t latency = 500;
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    subCb->recordSubscribeLatency(latency);
    folly::doNotOptimizeAway(latency);
  }
}

BENCHMARK(BM_StatsCollector_Snapshot, iters) {
  folly::BenchmarkSuspender susp;
  auto registry = std::make_shared<StatsRegistry>();
  auto collector = MoQStatsCollector::create_moq_stats_collector(registry);
  auto pubCb = collector->publisherCallback();
  // Populate some data
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
