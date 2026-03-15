#include <o_rly/ServiceMatcher.h>

#include <gtest/gtest.h>

namespace openmoq::o_rly {
namespace {

using namespace config;
using ME = ServiceConfig::MatchEntry;

// Helper to build a ServiceConfig with exact authorities (any path)
ServiceConfig makeExactService(
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
  return ServiceConfig{
      .name = std::move(name),
      .match = std::move(entries),
      .cache = cache,
  };
}

// Helper to build a ServiceConfig with wildcard authorities (any path)
ServiceConfig makeWildcardService(
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
  return ServiceConfig{
      .name = std::move(name),
      .match = std::move(entries),
      .cache = cache,
  };
}

ServiceConfig makeAnyAuthorityService(
    std::string name,
    ME::PathMatcher path = ME::PrefixPath{"/"},
    CacheConfig cache = {100, 3}
) {
  std::vector<ME> entries;
  entries.push_back(ME{
      .authority = ME::AnyAuthority{},
      .path = std::move(path),
  });
  return ServiceConfig{
      .name = std::move(name),
      .match = std::move(entries),
      .cache = cache,
  };
}

TEST(ServiceMatcher, ExactMatchHit) {
  std::vector<ServiceConfig> services = {
      makeExactService("svc1", {"live.example.com", "cdn.example.com"}),
  };
  ServiceMatcher matcher(services);

  EXPECT_EQ(matcher.match("live.example.com", ""), 0u);
  EXPECT_EQ(matcher.match("cdn.example.com", ""), 0u);
}

TEST(ServiceMatcher, ExactMatchMiss) {
  std::vector<ServiceConfig> services = {
      makeExactService("svc1", {"live.example.com"}),
  };
  ServiceMatcher matcher(services);

  EXPECT_EQ(matcher.match("other.example.com", ""), std::nullopt);
}

TEST(ServiceMatcher, WildcardSingleLabelMatch) {
  std::vector<ServiceConfig> services = {
      makeWildcardService("svc1", {"*.example.com"}),
  };
  ServiceMatcher matcher(services);

  EXPECT_EQ(matcher.match("foo.example.com", ""), 0u);
  EXPECT_EQ(matcher.match("bar.example.com", ""), 0u);
}

TEST(ServiceMatcher, WildcardRejectsMultiLabel) {
  std::vector<ServiceConfig> services = {
      makeWildcardService("svc1", {"*.example.com"}),
  };
  ServiceMatcher matcher(services);

  EXPECT_EQ(matcher.match("foo.bar.example.com", ""), std::nullopt);
}

TEST(ServiceMatcher, AnyAuthorityFallback) {
  std::vector<ServiceConfig> services = {
      makeExactService("svc1", {"live.example.com"}),
      makeAnyAuthorityService("any-svc"),
  };
  ServiceMatcher matcher(services);

  EXPECT_EQ(matcher.match("live.example.com", ""), 0u);
  EXPECT_EQ(matcher.match("unknown.com", ""), 1u);
  EXPECT_EQ(matcher.match("anything", ""), 1u);
}

TEST(ServiceMatcher, PriorityExactOverWildcardOverAny) {
  std::vector<ServiceConfig> services = {
      makeExactService("exact", {"foo.example.com"}),
      makeWildcardService("wild", {"*.example.com"}),
      makeAnyAuthorityService("any-svc"),
  };
  ServiceMatcher matcher(services);

  // Exact match wins
  EXPECT_EQ(matcher.match("foo.example.com", ""), 0u);
  // Wildcard match
  EXPECT_EQ(matcher.match("bar.example.com", ""), 1u);
  // Any-authority fallback
  EXPECT_EQ(matcher.match("other.net", ""), 2u);
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

  std::vector<ServiceConfig> services = {
      ServiceConfig{
          .name = "mixed",
          .match = std::move(entries),
          .cache = {100, 3},
      },
  };
  ServiceMatcher matcher(services);

  EXPECT_EQ(matcher.match("special.example.com", ""), 0u);
  EXPECT_EQ(matcher.match("stream.live.example.com", ""), 0u);
  EXPECT_EQ(matcher.match("unknown.com", ""), std::nullopt);
}

TEST(ServiceMatcher, EmptyAuthority) {
  std::vector<ServiceConfig> services = {
      makeExactService("svc1", {"live.example.com"}),
  };
  ServiceMatcher matcher(services);

  EXPECT_EQ(matcher.match("", ""), std::nullopt);
}

TEST(ServiceMatcher, WildcardSuffixOnly) {
  // Authority that is just the suffix without a label should not match
  std::vector<ServiceConfig> services = {
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

  std::vector<ServiceConfig> services = {
      ServiceConfig{.name = "svc", .match = std::move(entries), .cache = {100, 3}},
  };
  ServiceMatcher matcher(services);

  EXPECT_EQ(matcher.match("a.com", "/moq-relay"), 0u);
  EXPECT_EQ(matcher.match("a.com", "/other"), std::nullopt);
  EXPECT_EQ(matcher.match("a.com", "/moq-relay/extra"), std::nullopt);
}

TEST(ServiceMatcher, PrefixPathMatch) {
  std::vector<ME> entries;
  entries.push_back(ME{
      .authority = ME::ExactAuthority{"a.com"},
      .path = ME::PrefixPath{"/live/"},
  });

  std::vector<ServiceConfig> services = {
      ServiceConfig{.name = "svc", .match = std::move(entries), .cache = {100, 3}},
  };
  ServiceMatcher matcher(services);

  EXPECT_EQ(matcher.match("a.com", "/live/"), 0u);
  EXPECT_EQ(matcher.match("a.com", "/live/stream"), 0u);
  EXPECT_EQ(matcher.match("a.com", "/other"), std::nullopt);
}

TEST(ServiceMatcher, LongestPrefixWins) {
  // svc0 has /live/, svc1 has /live/hd/ — longer prefix should win
  std::vector<ServiceConfig> services = {
      ServiceConfig{
          .name = "short",
          .match = std::vector<ME>{ME{
              .authority = ME::ExactAuthority{"a.com"},
              .path = ME::PrefixPath{"/live/"},
          }},
          .cache = {100, 3},
      },
      ServiceConfig{
          .name = "long",
          .match = std::vector<ME>{ME{
              .authority = ME::ExactAuthority{"a.com"},
              .path = ME::PrefixPath{"/live/hd/"},
          }},
          .cache = {100, 3},
      },
  };
  ServiceMatcher matcher(services);

  EXPECT_EQ(matcher.match("a.com", "/live/hd/stream"), 1u);
  EXPECT_EQ(matcher.match("a.com", "/live/sd/stream"), 0u);
}

TEST(ServiceMatcher, ExactPathOverPrefix) {
  std::vector<ServiceConfig> services = {
      ServiceConfig{
          .name = "prefix",
          .match = std::vector<ME>{ME{
              .authority = ME::ExactAuthority{"a.com"},
              .path = ME::PrefixPath{"/relay"},
          }},
          .cache = {100, 3},
      },
      ServiceConfig{
          .name = "exact",
          .match = std::vector<ME>{ME{
              .authority = ME::ExactAuthority{"a.com"},
              .path = ME::ExactPath{"/relay"},
          }},
          .cache = {100, 3},
      },
  };
  ServiceMatcher matcher(services);

  // Exact path wins over prefix for exact match
  EXPECT_EQ(matcher.match("a.com", "/relay"), 1u);
  // Prefix still matches sub-paths
  EXPECT_EQ(matcher.match("a.com", "/relay/sub"), 0u);
}

TEST(ServiceMatcher, AnyPathFallback) {
  std::vector<ServiceConfig> services = {
      ServiceConfig{
          .name = "specific",
          .match = std::vector<ME>{ME{
              .authority = ME::ExactAuthority{"a.com"},
              .path = ME::ExactPath{"/moq"},
          }},
          .cache = {100, 3},
      },
      ServiceConfig{
          .name = "any",
          .match = std::vector<ME>{ME{
              .authority = ME::ExactAuthority{"a.com"},
              .path = ME::PrefixPath{"/"},
          }},
          .cache = {100, 3},
      },
  };
  ServiceMatcher matcher(services);

  EXPECT_EQ(matcher.match("a.com", "/moq"), 0u);
  EXPECT_EQ(matcher.match("a.com", "/anything"), 1u);
  EXPECT_EQ(matcher.match("a.com", ""), 1u);
}

TEST(ServiceMatcher, PathWithWildcardAuthority) {
  std::vector<ServiceConfig> services = {
      ServiceConfig{
          .name = "wild-path",
          .match = std::vector<ME>{ME{
              .authority = ME::WildcardAuthority{"*.example.com"},
              .path = ME::ExactPath{"/relay"},
          }},
          .cache = {100, 3},
      },
  };
  ServiceMatcher matcher(services);

  EXPECT_EQ(matcher.match("foo.example.com", "/relay"), 0u);
  EXPECT_EQ(matcher.match("foo.example.com", "/other"), std::nullopt);
}

TEST(ServiceMatcher, AnyAuthorityMatchesAnyPath) {
  std::vector<ServiceConfig> services = {
      makeAnyAuthorityService("any-svc"),
  };
  ServiceMatcher matcher(services);

  EXPECT_EQ(matcher.match("any.host", "/any/path"), 0u);
  EXPECT_EQ(matcher.match("other", ""), 0u);
}

TEST(ServiceMatcher, AuthorityMatchButPathMismatchFallsToWildcard) {
  std::vector<ServiceConfig> services = {
      ServiceConfig{
          .name = "exact-with-path",
          .match = std::vector<ME>{ME{
              .authority = ME::ExactAuthority{"foo.example.com"},
              .path = ME::ExactPath{"/specific"},
          }},
          .cache = {100, 3},
      },
      ServiceConfig{
          .name = "wild-any",
          .match = std::vector<ME>{ME{
              .authority = ME::WildcardAuthority{"*.example.com"},
              .path = ME::PrefixPath{"/"},
          }},
          .cache = {100, 3},
      },
  };
  ServiceMatcher matcher(services);

  // Exact authority matches but path doesn't → falls through to wildcard
  EXPECT_EQ(matcher.match("foo.example.com", "/specific"), 0u);
  EXPECT_EQ(matcher.match("foo.example.com", "/other"), 1u);
}

TEST(ServiceMatcher, MultipleAnyAuthorityDifferentPaths) {
  std::vector<ServiceConfig> services = {
      makeAnyAuthorityService("relay", ME::ExactPath{"/moq-relay"}),
      makeAnyAuthorityService("live", ME::PrefixPath{"/live/"}),
      makeAnyAuthorityService("default", ME::PrefixPath{"/"}),
  };
  ServiceMatcher matcher(services);

  EXPECT_EQ(matcher.match("anything.com", "/moq-relay"), 0u);
  EXPECT_EQ(matcher.match("anything.com", "/live/stream"), 1u);
  EXPECT_EQ(matcher.match("anything.com", "/other"), 2u);
}

TEST(ServiceMatcher, AnyAuthorityExactPathOverPrefix) {
  std::vector<ServiceConfig> services = {
      makeAnyAuthorityService("prefix", ME::PrefixPath{"/relay"}),
      makeAnyAuthorityService("exact", ME::ExactPath{"/relay"}),
  };
  ServiceMatcher matcher(services);

  // Exact path wins
  EXPECT_EQ(matcher.match("any.com", "/relay"), 1u);
  // Prefix still matches sub-paths
  EXPECT_EQ(matcher.match("any.com", "/relay/sub"), 0u);
}

TEST(ServiceMatcher, AuthorityMatchButPathMismatchFallsToAny) {
  std::vector<ServiceConfig> services = {
      ServiceConfig{
          .name = "exact-with-path",
          .match = std::vector<ME>{ME{
              .authority = ME::ExactAuthority{"foo.com"},
              .path = ME::ExactPath{"/specific"},
          }},
          .cache = {100, 3},
      },
      makeAnyAuthorityService("any-svc"),
  };
  ServiceMatcher matcher(services);

  // Exact authority matches but path doesn't → falls through to any-authority
  EXPECT_EQ(matcher.match("foo.com", "/specific"), 0u);
  EXPECT_EQ(matcher.match("foo.com", "/other"), 1u);
}

} // namespace
} // namespace openmoq::o_rly
