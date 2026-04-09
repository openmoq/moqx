#include <benchmark/benchmark.h>
#include <moqx/stats/MoQStatsCollector.h>

namespace {

using openmoq::moqx::stats::MoQStatsCollector;
using openmoq::moqx::stats::StatsRegistry;

void BM_StatsCollector_OnSubscribeSuccess(benchmark::State& state) {
  auto registry = std::make_shared<StatsRegistry>();
  auto collector = MoQStatsCollector::create_moq_stats_collector(registry);
  auto pubCb = collector->publisherCallback();
  for (auto _ : state) {
    pubCb->onSubscribeSuccess();
  }
}
BENCHMARK(BM_StatsCollector_OnSubscribeSuccess);

void BM_StatsCollector_OnSubscribeError(benchmark::State& state) {
  auto registry = std::make_shared<StatsRegistry>();
  auto collector = MoQStatsCollector::create_moq_stats_collector(registry);
  auto pubCb = collector->publisherCallback();
  for (auto _ : state) {
    pubCb->onSubscribeError(moxygen::RequestErrorCode::INTERNAL_ERROR);
  }
}
BENCHMARK(BM_StatsCollector_OnSubscribeError);

void BM_StatsCollector_RecordLatency(benchmark::State& state) {
  auto registry = std::make_shared<StatsRegistry>();
  auto collector = MoQStatsCollector::create_moq_stats_collector(registry);
  auto subCb = collector->subscriberCallback();
  uint64_t latency = 500;
  for (auto _ : state) {
    subCb->recordSubscribeLatency(latency);
    benchmark::DoNotOptimize(latency);
  }
}
BENCHMARK(BM_StatsCollector_RecordLatency);

void BM_StatsCollector_Snapshot(benchmark::State& state) {
  auto registry = std::make_shared<StatsRegistry>();
  auto collector = MoQStatsCollector::create_moq_stats_collector(registry);
  auto pubCb = collector->publisherCallback();
  // Populate some data
  for (int i = 0; i < 1000; ++i) {
    pubCb->onSubscribeSuccess();
    pubCb->onFetchSuccess();
  }
  for (auto _ : state) {
    auto snap = collector->snapshot();
    benchmark::DoNotOptimize(snap);
  }
}
BENCHMARK(BM_StatsCollector_Snapshot);

void BM_StatsCollector_SessionLifecycle(benchmark::State& state) {
  auto registry = std::make_shared<StatsRegistry>();
  auto collector = MoQStatsCollector::create_moq_stats_collector(registry);
  for (auto _ : state) {
    collector->onSessionStart();
    collector->onSessionEnd();
  }
}
BENCHMARK(BM_StatsCollector_SessionLifecycle);

} // namespace
