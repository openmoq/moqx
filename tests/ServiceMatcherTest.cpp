#include <o_rly/ServiceMatcher.h>

#include <gtest/gtest.h>

namespace openmoq::o_rly {
namespace {

using namespace config;
using ME = ServiceConfig::MatchEntry;

// Helper to build a ServiceConfig with exact authorities (any path)
std::pair<std::string, ServiceConfig> makeExactService(
    std::string name,
    std::vector<std::string> exactValues,
    CacheConfig cache = {100, 3}
) {
  std::vector<ME> entries;
  for (auto& v : exactValues) {
    entries.push_back(ME{
        .authority = ME::ExactAuthority{std::move(v)},
        .path = ME::PrefixPath{"/"},
    });
  }
  return {
      std::move(name),
      ServiceConfig{
          .match = std::move(entries),
          .cache = cache,
      }
  };
}

// Helper to build a ServiceConfig with wildcard authorities (any path)
std::pair<std::string, ServiceConfig> makeWildcardService(
    std::string name,
    std::vector<std::string> patterns,
    CacheConfig cache = {100, 3}
) {
  std::vector<ME> entries;
  for (auto& p : patterns) {
    entries.push_back(ME{
        .authority = ME::WildcardAuthority{std::move(p)},
        .path = ME::PrefixPath{"/"},
    });
  }
  return {
      std::move(name),
      ServiceConfig{
          .match = std::move(entries),
          .cache = cache,
      }
  };
}

std::pair<std::string, ServiceConfig> makeAnyAuthorityService(
    std::string name,
    ME::PathMatcher path = ME::PrefixPath{"/"},
    CacheConfig cache = {100, 3}
) {
  std::vector<ME> entries;
  entries.push_back(ME{
      .authority = ME::AnyAuthority{},
      .path = std::move(path),
  });
  return {
      std::move(name),
      ServiceConfig{
          .match = std::move(entries),
          .cache = cache,
      }
  };
}

TEST(ServiceMatcher, ExactMatchHit) {
  folly::F14FastMap<std::string, ServiceConfig> services = {
      makeExactService("svc1", {"live.example.com", "cdn.example.com"}),
  };
  ServiceMatcher matcher(services);

  EXPECT_EQ(matcher.match("live.example.com", ""), "svc1");
  EXPECT_EQ(matcher.match("cdn.example.com", ""), "svc1");
}

TEST(ServiceMatcher, ExactMatchMiss) {
  folly::F14FastMap<std::string, ServiceConfig> services = {
      makeExactService("svc1", {"live.example.com"}),
  };
  ServiceMatcher matcher(services);

  EXPECT_EQ(matcher.match("other.example.com", ""), std::nullopt);
}

TEST(ServiceMatcher, WildcardSingleLabelMatch) {
  folly::F14FastMap<std::string, ServiceConfig> services = {
      makeWildcardService("svc1", {"*.example.com"}),
  };
  ServiceMatcher matcher(services);

  EXPECT_EQ(matcher.match("foo.example.com", ""), "svc1");
  EXPECT_EQ(matcher.match("bar.example.com", ""), "svc1");
}

TEST(ServiceMatcher, WildcardRejectsMultiLabel) {
  folly::F14FastMap<std::string, ServiceConfig> services = {
      makeWildcardService("svc1", {"*.example.com"}),
  };
  ServiceMatcher matcher(services);

  EXPECT_EQ(matcher.match("foo.bar.example.com", ""), std::nullopt);
}

TEST(ServiceMatcher, AnyAuthorityFallback) {
  folly::F14FastMap<std::string, ServiceConfig> services = {
      makeExactService("svc1", {"live.example.com"}),
      makeAnyAuthorityService("any-svc"),
  };
  ServiceMatcher matcher(services);

  EXPECT_EQ(matcher.match("live.example.com", ""), "svc1");
  EXPECT_EQ(matcher.match("unknown.com", ""), "any-svc");
  EXPECT_EQ(matcher.match("anything", ""), "any-svc");
}

TEST(ServiceMatcher, PriorityExactOverWildcardOverAny) {
  folly::F14FastMap<std::string, ServiceConfig> services = {
      makeExactService("exact", {"foo.example.com"}),
      makeWildcardService("wild", {"*.example.com"}),
      makeAnyAuthorityService("any-svc"),
  };
  ServiceMatcher matcher(services);

  // Exact match wins
  EXPECT_EQ(matcher.match("foo.example.com", ""), "exact");
  // Wildcard match
  EXPECT_EQ(matcher.match("bar.example.com", ""), "wild");
  // Any-authority fallback
  EXPECT_EQ(matcher.match("other.net", ""), "any-svc");
}

TEST(ServiceMatcher, MixedMatchersOnOneService) {
  // A single service with both exact and wildcard entries
  std::vector<ME> entries;
  entries.push_back(ME{
      .authority = ME::ExactAuthority{"special.example.com"},
      .path = ME::PrefixPath{"/"},
  });
  entries.push_back(ME{
      .authority = ME::WildcardAuthority{"*.live.example.com"},
      .path = ME::PrefixPath{"/"},
  });

  folly::F14FastMap<std::string, ServiceConfig> services = {
      {"mixed",
       ServiceConfig{
           .match = std::move(entries),
           .cache = {100, 3},
       }},
  };
  ServiceMatcher matcher(services);

  EXPECT_EQ(matcher.match("special.example.com", ""), "mixed");
  EXPECT_EQ(matcher.match("stream.live.example.com", ""), "mixed");
  EXPECT_EQ(matcher.match("unknown.com", ""), std::nullopt);
}

TEST(ServiceMatcher, EmptyAuthority) {
  folly::F14FastMap<std::string, ServiceConfig> services = {
      makeExactService("svc1", {"live.example.com"}),
  };
  ServiceMatcher matcher(services);

  EXPECT_EQ(matcher.match("", ""), std::nullopt);
}

TEST(ServiceMatcher, WildcardSuffixOnly) {
  // Authority that is just the suffix without a label should not match
  folly::F14FastMap<std::string, ServiceConfig> services = {
      makeWildcardService("svc1", {"*.example.com"}),
  };
  ServiceMatcher matcher(services);

  EXPECT_EQ(matcher.match("example.com", ""), std::nullopt);
}

// --- Path matching tests ---

TEST(ServiceMatcher, ExactPathMatch) {
  std::vector<ME> entries;
  entries.push_back(ME{
      .authority = ME::ExactAuthority{"a.com"},
      .path = ME::ExactPath{"/moq-relay"},
  });

  folly::F14FastMap<std::string, ServiceConfig> services = {
      {"svc", ServiceConfig{.match = std::move(entries), .cache = {100, 3}}},
  };
  ServiceMatcher matcher(services);

  EXPECT_EQ(matcher.match("a.com", "/moq-relay"), "svc");
  EXPECT_EQ(matcher.match("a.com", "/other"), std::nullopt);
  EXPECT_EQ(matcher.match("a.com", "/moq-relay/extra"), std::nullopt);
}

TEST(ServiceMatcher, PrefixPathMatch) {
  std::vector<ME> entries;
  entries.push_back(ME{
      .authority = ME::ExactAuthority{"a.com"},
      .path = ME::PrefixPath{"/live/"},
  });

  folly::F14FastMap<std::string, ServiceConfig> services = {
      {"svc", ServiceConfig{.match = std::move(entries), .cache = {100, 3}}},
  };
  ServiceMatcher matcher(services);

  EXPECT_EQ(matcher.match("a.com", "/live/"), "svc");
  EXPECT_EQ(matcher.match("a.com", "/live/stream"), "svc");
  EXPECT_EQ(matcher.match("a.com", "/other"), std::nullopt);
}

TEST(ServiceMatcher, LongestPrefixWins) {
  // svc0 has /live/, svc1 has /live/hd/ — longer prefix should win
  folly::F14FastMap<std::string, ServiceConfig> services = {
      {"short",
       ServiceConfig{
           .match = std::vector<ME>{ME{
               .authority = ME::ExactAuthority{"a.com"},
               .path = ME::PrefixPath{"/live/"},
           }},
           .cache = {100, 3},
       }},
      {"long",
       ServiceConfig{
           .match = std::vector<ME>{ME{
               .authority = ME::ExactAuthority{"a.com"},
               .path = ME::PrefixPath{"/live/hd/"},
           }},
           .cache = {100, 3},
       }},
  };
  ServiceMatcher matcher(services);

  EXPECT_EQ(matcher.match("a.com", "/live/hd/stream"), "long");
  EXPECT_EQ(matcher.match("a.com", "/live/sd/stream"), "short");
}

TEST(ServiceMatcher, ExactPathOverPrefix) {
  folly::F14FastMap<std::string, ServiceConfig> services = {
      {"prefix",
       ServiceConfig{
           .match = std::vector<ME>{ME{
               .authority = ME::ExactAuthority{"a.com"},
               .path = ME::PrefixPath{"/relay"},
           }},
           .cache = {100, 3},
       }},
      {"exact",
       ServiceConfig{
           .match = std::vector<ME>{ME{
               .authority = ME::ExactAuthority{"a.com"},
               .path = ME::ExactPath{"/relay"},
           }},
           .cache = {100, 3},
       }},
  };
  ServiceMatcher matcher(services);

  // Exact path wins over prefix for exact match
  EXPECT_EQ(matcher.match("a.com", "/relay"), "exact");
  // Prefix still matches sub-paths
  EXPECT_EQ(matcher.match("a.com", "/relay/sub"), "prefix");
}

TEST(ServiceMatcher, AnyPathFallback) {
  folly::F14FastMap<std::string, ServiceConfig> services = {
      {"specific",
       ServiceConfig{
           .match = std::vector<ME>{ME{
               .authority = ME::ExactAuthority{"a.com"},
               .path = ME::ExactPath{"/moq"},
           }},
           .cache = {100, 3},
       }},
      {"any",
       ServiceConfig{
           .match = std::vector<ME>{ME{
               .authority = ME::ExactAuthority{"a.com"},
               .path = ME::PrefixPath{"/"},
           }},
           .cache = {100, 3},
       }},
  };
  ServiceMatcher matcher(services);

  EXPECT_EQ(matcher.match("a.com", "/moq"), "specific");
  EXPECT_EQ(matcher.match("a.com", "/anything"), "any");
  EXPECT_EQ(matcher.match("a.com", ""), "any");
}

TEST(ServiceMatcher, PathWithWildcardAuthority) {
  folly::F14FastMap<std::string, ServiceConfig> services = {
      {"wild-path",
       ServiceConfig{
           .match = std::vector<ME>{ME{
               .authority = ME::WildcardAuthority{"*.example.com"},
               .path = ME::ExactPath{"/relay"},
           }},
           .cache = {100, 3},
       }},
  };
  ServiceMatcher matcher(services);

  EXPECT_EQ(matcher.match("foo.example.com", "/relay"), "wild-path");
  EXPECT_EQ(matcher.match("foo.example.com", "/other"), std::nullopt);
}

TEST(ServiceMatcher, AnyAuthorityMatchesAnyPath) {
  folly::F14FastMap<std::string, ServiceConfig> services = {
      makeAnyAuthorityService("any-svc"),
  };
  ServiceMatcher matcher(services);

  EXPECT_EQ(matcher.match("any.host", "/any/path"), "any-svc");
  EXPECT_EQ(matcher.match("other", ""), "any-svc");
}

TEST(ServiceMatcher, AuthorityMatchButPathMismatchFallsToWildcard) {
  folly::F14FastMap<std::string, ServiceConfig> services = {
      {"exact-with-path",
       ServiceConfig{
           .match = std::vector<ME>{ME{
               .authority = ME::ExactAuthority{"foo.example.com"},
               .path = ME::ExactPath{"/specific"},
           }},
           .cache = {100, 3},
       }},
      {"wild-any",
       ServiceConfig{
           .match = std::vector<ME>{ME{
               .authority = ME::WildcardAuthority{"*.example.com"},
               .path = ME::PrefixPath{"/"},
           }},
           .cache = {100, 3},
       }},
  };
  ServiceMatcher matcher(services);

  // Exact authority matches but path doesn't → falls through to wildcard
  EXPECT_EQ(matcher.match("foo.example.com", "/specific"), "exact-with-path");
  EXPECT_EQ(matcher.match("foo.example.com", "/other"), "wild-any");
}

TEST(ServiceMatcher, MultipleAnyAuthorityDifferentPaths) {
  folly::F14FastMap<std::string, ServiceConfig> services = {
      makeAnyAuthorityService("relay", ME::ExactPath{"/moq-relay"}),
      makeAnyAuthorityService("live", ME::PrefixPath{"/live/"}),
      makeAnyAuthorityService("default", ME::PrefixPath{"/"}),
  };
  ServiceMatcher matcher(services);

  EXPECT_EQ(matcher.match("anything.com", "/moq-relay"), "relay");
  EXPECT_EQ(matcher.match("anything.com", "/live/stream"), "live");
  EXPECT_EQ(matcher.match("anything.com", "/other"), "default");
}

TEST(ServiceMatcher, AnyAuthorityExactPathOverPrefix) {
  folly::F14FastMap<std::string, ServiceConfig> services = {
      makeAnyAuthorityService("prefix", ME::PrefixPath{"/relay"}),
      makeAnyAuthorityService("exact", ME::ExactPath{"/relay"}),
  };
  ServiceMatcher matcher(services);

  // Exact path wins
  EXPECT_EQ(matcher.match("any.com", "/relay"), "exact");
  // Prefix still matches sub-paths
  EXPECT_EQ(matcher.match("any.com", "/relay/sub"), "prefix");
}

TEST(ServiceMatcher, AuthorityMatchButPathMismatchFallsToAny) {
  folly::F14FastMap<std::string, ServiceConfig> services = {
      {"exact-with-path",
       ServiceConfig{
           .match = std::vector<ME>{ME{
               .authority = ME::ExactAuthority{"foo.com"},
               .path = ME::ExactPath{"/specific"},
           }},
           .cache = {100, 3},
       }},
      makeAnyAuthorityService("any-svc"),
  };
  ServiceMatcher matcher(services);

  // Exact authority matches but path doesn't → falls through to any-authority
  EXPECT_EQ(matcher.match("foo.com", "/specific"), "exact-with-path");
  EXPECT_EQ(matcher.match("foo.com", "/other"), "any-svc");
}

} // namespace
} // namespace openmoq::o_rly
