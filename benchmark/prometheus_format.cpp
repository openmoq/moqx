#include <benchmark/benchmark.h>
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

void BM_FormatPrometheus_1Session(benchmark::State& state) {
  auto snap = makeSnapshot(1);
  for (auto _ : state) {
    auto buf = StatsSnapshot::formatPrometheus(snap);
    benchmark::DoNotOptimize(buf);
  }
}
BENCHMARK(BM_FormatPrometheus_1Session);

void BM_FormatPrometheus_10Sessions(benchmark::State& state) {
  auto snap = makeSnapshot(10);
  for (auto _ : state) {
    auto buf = StatsSnapshot::formatPrometheus(snap);
    benchmark::DoNotOptimize(buf);
  }
}
BENCHMARK(BM_FormatPrometheus_10Sessions);

void BM_FormatPrometheus_100Sessions(benchmark::State& state) {
  auto snap = makeSnapshot(100);
  for (auto _ : state) {
    auto buf = StatsSnapshot::formatPrometheus(snap);
    benchmark::DoNotOptimize(buf);
  }
}
BENCHMARK(BM_FormatPrometheus_100Sessions);

void BM_FormatPrometheus_1000Sessions(benchmark::State& state) {
  auto snap = makeSnapshot(1000);
  for (auto _ : state) {
    auto buf = StatsSnapshot::formatPrometheus(snap);
    benchmark::DoNotOptimize(buf);
  }
}
BENCHMARK(BM_FormatPrometheus_1000Sessions);

} // namespace
