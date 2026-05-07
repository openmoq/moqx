#include <folly/Benchmark.h>
#include "stats/BoundedHistogram.h"
#include "stats/StatsRegistry.h"

namespace {

using openmoq::moqx::stats::BoundedHistogram;
using openmoq::moqx::stats::kLatencyBucketsUs;

BENCHMARK(BM_Histogram_AddValue_Hit, iters) {
  folly::BenchmarkSuspender susp;
  BoundedHistogram<kLatencyBucketsUs.size()> hist(kLatencyBucketsUs);
  uint64_t val = 500; // hits bucket 4 (<=500)
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    hist.addValue(val);
    folly::doNotOptimizeAway(hist.count);
  }
}

BENCHMARK(BM_Histogram_AddValue_Miss, iters) {
  folly::BenchmarkSuspender susp;
  BoundedHistogram<kLatencyBucketsUs.size()> hist(kLatencyBucketsUs);
  uint64_t val = 999999; // falls through to +Inf
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    hist.addValue(val);
    folly::doNotOptimizeAway(hist.count);
  }
}

BENCHMARK(BM_Histogram_FillCumulative, iters) {
  folly::BenchmarkSuspender susp;
  BoundedHistogram<kLatencyBucketsUs.size()> hist(kLatencyBucketsUs);
  // Populate with some data
  for (uint64_t j = 0; j < 10000; ++j) {
    hist.addValue(j * 10);
  }
  std::array<uint64_t, kLatencyBucketsUs.size() + 1> cumulative{};
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    hist.fillCumulative(cumulative);
    folly::doNotOptimizeAway(cumulative);
  }
}

} // namespace
