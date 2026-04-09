#include <benchmark/benchmark.h>
#include <moqx/stats/BoundedHistogram.h>
#include <moqx/stats/StatsRegistry.h>

namespace {

using openmoq::moqx::stats::BoundedHistogram;
using openmoq::moqx::stats::kLatencyBucketsUs;

void BM_Histogram_AddValue_Hit(benchmark::State& state) {
  BoundedHistogram<kLatencyBucketsUs.size()> hist(kLatencyBucketsUs);
  uint64_t val = 500; // hits bucket 4 (<=500)
  for (auto _ : state) {
    hist.addValue(val);
    benchmark::DoNotOptimize(hist.count);
  }
}
BENCHMARK(BM_Histogram_AddValue_Hit);

void BM_Histogram_AddValue_Miss(benchmark::State& state) {
  BoundedHistogram<kLatencyBucketsUs.size()> hist(kLatencyBucketsUs);
  uint64_t val = 999999; // falls through to +Inf
  for (auto _ : state) {
    hist.addValue(val);
    benchmark::DoNotOptimize(hist.count);
  }
}
BENCHMARK(BM_Histogram_AddValue_Miss);

void BM_Histogram_FillCumulative(benchmark::State& state) {
  BoundedHistogram<kLatencyBucketsUs.size()> hist(kLatencyBucketsUs);
  // Populate with some data
  for (uint64_t i = 0; i < 10000; ++i) {
    hist.addValue(i * 10);
  }
  std::array<uint64_t, kLatencyBucketsUs.size() + 1> cumulative{};
  for (auto _ : state) {
    hist.fillCumulative(cumulative);
    benchmark::DoNotOptimize(cumulative);
  }
}
BENCHMARK(BM_Histogram_FillCumulative);

} // namespace
