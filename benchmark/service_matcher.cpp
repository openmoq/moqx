#include <benchmark/benchmark.h>
#include <moqx/ServiceMatcher.h>

namespace {

using openmoq::moqx::ServiceMatcher;
using openmoq::moqx::config::ServiceConfig;

// Build a realistic service config with multiple services and match rules.
folly::F14FastMap<std::string, ServiceConfig> makeTestServices() {
  folly::F14FastMap<std::string, ServiceConfig> services;

  // Service with exact authority + exact path
  {
    ServiceConfig svc;
    svc.cache = {100, 3};
    ServiceConfig::MatchEntry entry;
    entry.authority = ServiceConfig::MatchEntry::ExactAuthority{"relay.example.com"};
    entry.path = ServiceConfig::MatchEntry::ExactPath{"/moq-relay"};
    svc.match.push_back(entry);
    services["production"] = std::move(svc);
  }

  // Service with wildcard authority + prefix path
  {
    ServiceConfig svc;
    svc.cache = {50, 3};
    ServiceConfig::MatchEntry entry;
    entry.authority = ServiceConfig::MatchEntry::WildcardAuthority{"*.test.example.com"};
    entry.path = ServiceConfig::MatchEntry::PrefixPath{"/test/"};
    svc.match.push_back(entry);
    services["testing"] = std::move(svc);
  }

  // Catch-all service
  {
    ServiceConfig svc;
    svc.cache = {10, 3};
    ServiceConfig::MatchEntry entry;
    entry.authority = ServiceConfig::MatchEntry::AnyAuthority{};
    entry.path = ServiceConfig::MatchEntry::PrefixPath{"/"};
    svc.match.push_back(entry);
    services["default"] = std::move(svc);
  }

  return services;
}

void BM_ServiceMatcher_ExactMatch(benchmark::State& state) {
  auto services = makeTestServices();
  ServiceMatcher matcher(services);
  for (auto _ : state) {
    auto result = matcher.match("relay.example.com", "/moq-relay");
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_ServiceMatcher_ExactMatch);

void BM_ServiceMatcher_WildcardMatch(benchmark::State& state) {
  auto services = makeTestServices();
  ServiceMatcher matcher(services);
  for (auto _ : state) {
    auto result = matcher.match("foo.test.example.com", "/test/bar");
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_ServiceMatcher_WildcardMatch);

void BM_ServiceMatcher_FallbackMatch(benchmark::State& state) {
  auto services = makeTestServices();
  ServiceMatcher matcher(services);
  for (auto _ : state) {
    auto result = matcher.match("unknown.host", "/something");
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_ServiceMatcher_FallbackMatch);

void BM_ServiceMatcher_NoMatch(benchmark::State& state) {
  // Empty services — nothing matches
  folly::F14FastMap<std::string, ServiceConfig> empty;
  ServiceMatcher matcher(empty);
  for (auto _ : state) {
    auto result = matcher.match("any.host", "/any/path");
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_ServiceMatcher_NoMatch);

} // namespace
