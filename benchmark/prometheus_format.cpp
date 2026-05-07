#include <folly/Benchmark.h>
#include <moqx/stats/StatsRegistry.h>

namespace {

using openmoq::moqx::stats::StatsSnapshot;

// Build a snapshot with varying amounts of populated data.
static StatsSnapshot makeSnapshot(int sessions) {
  StatsSnapshot snap{};
  snap.pubSubscribeSuccess = sessions * 10;
  snap.pubSubscribeError = sessions;
  snap.pubFetchSuccess = sessions * 5;
  snap.subSubscribeSuccess = sessions * 8;
  snap.quicPacketsReceived = sessions * 50000;
  snap.quicPacketsSent = sessions * 48000;
  snap.quicBytesRead = sessions * 10000000;
  snap.quicBytesWritten = sessions * 9500000;
  snap.moqActiveSessions = sessions;
  snap.quicActiveConnections = sessions;
  snap.pubActiveSubscriptions = sessions * 3;
  snap.moqSubscribeLatencyCount = sessions * 10;
  snap.moqSubscribeLatencySum = sessions * 5000;
  snap.moqFetchLatencyCount = sessions * 5;
  snap.moqFetchLatencySum = sessions * 3000;
  return snap;
}

void BM_FormatPrometheus(unsigned iters, int sessions) {
  folly::BenchmarkSuspender susp;
  auto snap = makeSnapshot(sessions);
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    auto buf = StatsSnapshot::formatPrometheus(snap);
    folly::doNotOptimizeAway(buf);
  }
}
BENCHMARK_NAMED_PARAM(BM_FormatPrometheus, _1Session, 1)
BENCHMARK_NAMED_PARAM(BM_FormatPrometheus, _10Sessions, 10)
BENCHMARK_NAMED_PARAM(BM_FormatPrometheus, _100Sessions, 100)
BENCHMARK_NAMED_PARAM(BM_FormatPrometheus, _1000Sessions, 1000)

} // namespace
