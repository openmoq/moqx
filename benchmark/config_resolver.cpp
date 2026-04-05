#include <benchmark/benchmark.h>
#include <moqx/config/loader/config_resolver.h>
#include <moqx/config/loader/parsed_config.h>

namespace {

using namespace openmoq::moqx::config;

// Build a minimal ParsedConfig programmatically (no YAML I/O).
static ParsedConfig makeMinimalConfig() {
  ParsedConfig cfg;

  ParsedListenerConfig listener;
  listener.name.set("main");
  listener.udp.set(ParsedUdpConfig{ParsedSocketConfig{{"0.0.0.0"}, {4433}}});
  listener.tls.set(ParsedListenerTlsConfig{std::nullopt, std::nullopt, {true}});
  listener.endpoint.set("/moq-relay");
  cfg.listeners.set({listener});

  ParsedServiceConfig svc;
  ParsedServiceConfig::MatchRule rule;
  rule.authority.set(
      ParsedServiceConfig::MatchRule::AnyAuthority{{true}});
  rule.path.set(
      ParsedServiceConfig::MatchRule::PrefixPath{{"/"}}
  );
  svc.match.set({rule});

  cfg.services.set({{"default", svc}});

  return cfg;
}

static ParsedConfig makeManyServicesConfig(int n) {
  auto cfg = makeMinimalConfig();
  std::map<std::string, ParsedServiceConfig> services;
  for (int i = 0; i < n; ++i) {
    ParsedServiceConfig svc;
    ParsedServiceConfig::MatchRule rule;
    rule.authority.set(
        ParsedServiceConfig::MatchRule::ExactAuthority{
            {"svc" + std::to_string(i) + ".example.com"}});
    rule.path.set(
        ParsedServiceConfig::MatchRule::PrefixPath{{"/"}}
    );
    svc.match.set({rule});
    services["svc" + std::to_string(i)] = svc;
  }
  cfg.services.set(services);
  return cfg;
}

void BM_ConfigResolve_Minimal(benchmark::State& state) {
  auto parsed = makeMinimalConfig();
  for (auto _ : state) {
    auto resolved = resolveConfig(parsed);
    benchmark::DoNotOptimize(resolved);
  }
}
BENCHMARK(BM_ConfigResolve_Minimal);

void BM_ConfigResolve_10Services(benchmark::State& state) {
  auto parsed = makeManyServicesConfig(10);
  for (auto _ : state) {
    auto resolved = resolveConfig(parsed);
    benchmark::DoNotOptimize(resolved);
  }
}
BENCHMARK(BM_ConfigResolve_10Services);

void BM_ConfigResolve_50Services(benchmark::State& state) {
  auto parsed = makeManyServicesConfig(50);
  for (auto _ : state) {
    auto resolved = resolveConfig(parsed);
    benchmark::DoNotOptimize(resolved);
  }
}
BENCHMARK(BM_ConfigResolve_50Services);

} // namespace
