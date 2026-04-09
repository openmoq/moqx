#include <benchmark/benchmark.h>
#include <moqx/config/loader/loader.h>
#include <moqx/config/loader/config_resolver.h>

#include <fstream>

namespace {

using openmoq::moqx::config::loadConfig;
using openmoq::moqx::config::resolveConfig;
using openmoq::moqx::config::generateSchema;

// Write a temporary YAML config file for benchmarking.
static std::string writeTempConfig(int numServices) {
  std::string path = "/tmp/moqx_bench_config.yaml";
  std::ofstream out(path);
  out << "listeners:\n"
      << "  - name: main\n"
      << "    udp:\n"
      << "      socket:\n"
      << "        address: \"0.0.0.0\"\n"
      << "        port: 4433\n"
      << "    tls:\n"
      << "      insecure: true\n"
      << "    endpoint: /moq-relay\n"
      << "\n"
      << "service_defaults:\n"
      << "  cache:\n"
      << "    enabled: true\n"
      << "    max_tracks: 100\n"
      << "    max_groups_per_track: 3\n"
      << "\n"
      << "services:\n";
  for (int i = 0; i < numServices; ++i) {
    out << "  svc" << i << ":\n"
        << "    match:\n"
        << "      - authority:\n"
        << "          exact: svc" << i << ".example.com\n"
        << "        path:\n"
        << "          prefix: /\n";
  }
  out.flush();
  return path;
}

void BM_ConfigLoad_Small(benchmark::State& state) {
  auto path = writeTempConfig(3);
  for (auto _ : state) {
    auto config = loadConfig(path);
    benchmark::DoNotOptimize(config);
  }
}
BENCHMARK(BM_ConfigLoad_Small);

void BM_ConfigLoad_Large(benchmark::State& state) {
  auto path = writeTempConfig(50);
  for (auto _ : state) {
    auto config = loadConfig(path);
    benchmark::DoNotOptimize(config);
  }
}
BENCHMARK(BM_ConfigLoad_Large);

void BM_ConfigResolve(benchmark::State& state) {
  auto path = writeTempConfig(10);
  auto parsed = loadConfig(path);
  for (auto _ : state) {
    auto resolved = resolveConfig(parsed);
    benchmark::DoNotOptimize(resolved);
  }
}
BENCHMARK(BM_ConfigResolve);

void BM_ConfigGenerateSchema(benchmark::State& state) {
  for (auto _ : state) {
    auto schema = generateSchema();
    benchmark::DoNotOptimize(schema);
  }
}
BENCHMARK(BM_ConfigGenerateSchema);

} // namespace
