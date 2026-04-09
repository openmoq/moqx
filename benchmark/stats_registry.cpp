#include <benchmark/benchmark.h>
#include <moqx/stats/StatsRegistry.h>

namespace {

using openmoq::moqx::stats::kLatencyBucketsUs;
using openmoq::moqx::stats::requestErrorCodeIndex;
using openmoq::moqx::stats::StatsSnapshot;

void BM_StatsSnapshot_Accumulate(benchmark::State& state) {
  StatsSnapshot base{};
  StatsSnapshot delta{};
  delta.pubSubscribeSuccess = 1;
  delta.quicPacketsReceived = 100;
  delta.moqActiveSessions = 5;
  for (auto _ : state) {
    base += delta;
    benchmark::DoNotOptimize(base);
  }
}
BENCHMARK(BM_StatsSnapshot_Accumulate);

void BM_StatsSnapshot_FormatPrometheus(benchmark::State& state) {
  StatsSnapshot snap{};
  snap.pubSubscribeSuccess = 42;
  snap.quicPacketsReceived = 10000;
  snap.quicPacketsSent = 9500;
  snap.moqActiveSessions = 3;
  snap.quicActiveConnections = 12;
  snap.moqSubscribeLatencyCount = 100;
  snap.moqSubscribeLatencySum = 50000;
  for (auto _ : state) {
    auto buf = StatsSnapshot::formatPrometheus(snap);
    benchmark::DoNotOptimize(buf);
  }
}
BENCHMARK(BM_StatsSnapshot_FormatPrometheus);

void BM_RequestErrorCodeIndex(benchmark::State& state) {
  auto code = moxygen::RequestErrorCode::TRACK_NOT_EXIST;
  for (auto _ : state) {
    auto idx = requestErrorCodeIndex(code);
    benchmark::DoNotOptimize(idx);
  }
}
BENCHMARK(BM_RequestErrorCodeIndex);

} // namespace
