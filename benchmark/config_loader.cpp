#include <folly/Benchmark.h>
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

BENCHMARK(BM_ConfigLoad_Small, iters) {
  folly::BenchmarkSuspender susp;
  auto path = writeTempConfig(3);
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    auto config = loadConfig(path);
    folly::doNotOptimizeAway(config);
  }
}

BENCHMARK(BM_ConfigLoad_Large, iters) {
  folly::BenchmarkSuspender susp;
  auto path = writeTempConfig(50);
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    auto config = loadConfig(path);
    folly::doNotOptimizeAway(config);
  }
}

BENCHMARK(BM_ConfigResolve, iters) {
  folly::BenchmarkSuspender susp;
  auto path = writeTempConfig(10);
  auto parsed = loadConfig(path);
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    auto resolved = resolveConfig(parsed);
    folly::doNotOptimizeAway(resolved);
  }
}

BENCHMARK(BM_ConfigGenerateSchema, iters) {
  for (unsigned i = 0; i < iters; ++i) {
    auto schema = generateSchema();
    folly::doNotOptimizeAway(schema);
  }
}

} // namespace
