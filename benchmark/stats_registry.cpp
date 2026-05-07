#include <folly/Benchmark.h>
#include <moqx/stats/StatsRegistry.h>

namespace {

using openmoq::moqx::stats::kLatencyBucketsUs;
using openmoq::moqx::stats::requestErrorCodeIndex;
using openmoq::moqx::stats::StatsSnapshot;

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

BENCHMARK(BM_StatsSnapshot_FormatPrometheus, iters) {
  folly::BenchmarkSuspender susp;
  StatsSnapshot snap{};
  snap.pubSubscribeSuccess = 42;
  snap.quicPacketsReceived = 10000;
  snap.quicPacketsSent = 9500;
  snap.moqActiveSessions = 3;
  snap.quicActiveConnections = 12;
  snap.moqSubscribeLatencyCount = 100;
  snap.moqSubscribeLatencySum = 50000;
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    auto buf = StatsSnapshot::formatPrometheus(snap);
    folly::doNotOptimizeAway(buf);
  }
}

BENCHMARK(BM_RequestErrorCodeIndex, iters) {
  folly::BenchmarkSuspender susp;
  auto code = moxygen::RequestErrorCode::TRACK_NOT_EXIST;
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    auto idx = requestErrorCodeIndex(code);
    folly::doNotOptimizeAway(idx);
  }
}

} // namespace
