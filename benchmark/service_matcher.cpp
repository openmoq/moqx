#include <folly/Benchmark.h>
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

BENCHMARK(BM_ServiceMatcher_ExactMatch, iters) {
  folly::BenchmarkSuspender susp;
  auto services = makeTestServices();
  ServiceMatcher matcher(services);
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    auto result = matcher.match("relay.example.com", "/moq-relay");
    folly::doNotOptimizeAway(result);
  }
}

BENCHMARK(BM_ServiceMatcher_WildcardMatch, iters) {
  folly::BenchmarkSuspender susp;
  auto services = makeTestServices();
  ServiceMatcher matcher(services);
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    auto result = matcher.match("foo.test.example.com", "/test/bar");
    folly::doNotOptimizeAway(result);
  }
}

BENCHMARK(BM_ServiceMatcher_FallbackMatch, iters) {
  folly::BenchmarkSuspender susp;
  auto services = makeTestServices();
  ServiceMatcher matcher(services);
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    auto result = matcher.match("unknown.host", "/something");
    folly::doNotOptimizeAway(result);
  }
}

BENCHMARK(BM_ServiceMatcher_NoMatch, iters) {
  folly::BenchmarkSuspender susp;
  // Empty services — nothing matches
  folly::F14FastMap<std::string, ServiceConfig> empty;
  ServiceMatcher matcher(empty);
  susp.dismiss();
  for (unsigned i = 0; i < iters; ++i) {
    auto result = matcher.match("any.host", "/any/path");
    folly::doNotOptimizeAway(result);
  }
}

} // namespace
