#include <o_rly/config/loader/config_resolver.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace openmoq::o_rly::config {
namespace {

using ::testing::HasSubstr;
using ::testing::IsEmpty;

using AuthMatch = ParsedServiceConfig::MatchRule::AuthorityMatch;
using PMatch = ParsedServiceConfig::MatchRule::PathMatch;

// Shorthand for the common "match any path" path matcher.
PMatch anyPath() {
  return PMatch{ParsedServiceConfig::MatchRule::PrefixPath{"/"}};
}

ParsedCacheConfig makeDefaultCache() {
  ParsedCacheConfig cache;
  cache.enabled = std::optional<bool>{true};
  cache.max_tracks = std::optional<uint32_t>{100};
  cache.max_groups_per_track = std::optional<uint32_t>{3};
  return cache;
}

ParsedAdminConfig makeDefaultAdmin() {
  ParsedAdminConfig admin;
  admin.port = uint16_t{9669};
  admin.address = std::string{"::"};
  admin.plaintext = true;
  return admin;
}

ParsedAdminConfig makeAdminWithTls(std::string cert = "/cert.pem", std::string key = "/key.pem") {
  ParsedAdminConfig admin;
  admin.port = uint16_t{9669};
  admin.address = std::string{"::"};
  admin.plaintext = false;
  ParsedAdminTlsConfig tls;
  tls.cert_file = std::move(cert);
  tls.key_file = std::move(key);
  admin.tls = std::optional<ParsedAdminTlsConfig>{std::move(tls)};
  return admin;
}

ParsedServiceConfig makeDefaultService() {
  ParsedServiceConfig svc;
  ParsedServiceConfig::MatchRule entry;
  entry.authority = AuthMatch{ParsedServiceConfig::MatchRule::AnyAuthority{true}};
  entry.path = anyPath();
  svc.match.value().push_back(std::move(entry));
  svc.cache = makeDefaultCache();
  return svc;
}

// Build a minimal valid insecure config with one any-authority service and admin.
ParsedConfig makeMinimalInsecureConfig(std::string name = "test") {
  ParsedConfig cfg;
  ParsedListenerConfig lc;
  lc.name = std::move(name);
  ParsedSocketConfig sock;
  sock.address = std::string("::");
  sock.port = uint16_t{9668};
  ParsedUdpConfig udp;
  udp.socket = std::move(sock);
  lc.udp = std::move(udp);
  ParsedListenerTlsConfig tls;
  tls.insecure = true;
  lc.tls = std::move(tls);
  lc.endpoint = std::string("/moq-relay");
  cfg.listeners.value().push_back(std::move(lc));
  cfg.services.value().emplace("default", makeDefaultService());
  cfg.admin = std::optional<ParsedAdminConfig>{makeDefaultAdmin()};
  return cfg;
}

// Helper to make a service with authority match entries
ParsedServiceConfig makeAuthorityService(std::vector<ParsedServiceConfig::MatchRule> matchEntries) {
  ParsedServiceConfig svc;
  svc.match.value() = std::move(matchEntries);
  svc.cache = makeDefaultCache();
  return svc;
}

ParsedServiceConfig::MatchRule
makeExactAuthorityMatch(std::string authority, PMatch path = anyPath()) {
  ParsedServiceConfig::MatchRule entry;
  entry.authority = AuthMatch{ParsedServiceConfig::MatchRule::ExactAuthority{std::move(authority)}};
  entry.path = std::move(path);
  return entry;
}

ParsedServiceConfig::MatchRule
makeWildcardAuthorityMatch(std::string pattern, PMatch path = anyPath()) {
  ParsedServiceConfig::MatchRule entry;
  entry.authority =
      AuthMatch{ParsedServiceConfig::MatchRule::WildcardAuthority{std::move(pattern)}};
  entry.path = std::move(path);
  return entry;
}

ParsedServiceConfig::MatchRule makeAnyAuthorityMatch(PMatch path = anyPath()) {
  ParsedServiceConfig::MatchRule entry;
  entry.authority = AuthMatch{ParsedServiceConfig::MatchRule::AnyAuthority{true}};
  entry.path = std::move(path);
  return entry;
}

// --- Validation error tests ---

TEST(ResolveConfig, NoListeners) {
  ParsedConfig cfg;
  cfg.services.value().emplace("default", makeDefaultService());
  cfg.admin = std::optional<ParsedAdminConfig>{makeDefaultAdmin()};
  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("At least one listener"));
}

TEST(ResolveConfig, InsecureFalseNoCerts) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.listeners.value()[0].tls.value().insecure = false;

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("cert_file"));
}

TEST(ResolveConfig, PortZero) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.listeners.value()[0].udp.value().socket.value().port = uint16_t{0};

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("port"));
}

TEST(ResolveConfig, InsecureWithCertsWarning) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.listeners.value()[0].tls.value().cert_file = std::string("/some/cert.pem");
  cfg.listeners.value()[0].tls.value().key_file = std::string("/some/key.pem");

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  ASSERT_FALSE(result.value().warnings.empty());
  EXPECT_THAT(result.value().warnings[0], HasSubstr("ignored"));
}

// --- Service validation error tests ---

TEST(ResolveConfig, NoServices) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.services.value().clear();

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("At least one service"));
}

TEST(ResolveConfig, DuplicateExactAuthorityAcrossServices) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.services.value().clear();

  cfg.services.value().emplace(
      "svc1",
      makeAuthorityService({makeExactAuthorityMatch("live.example.com")})
  );
  cfg.services.value().emplace(
      "svc2",
      makeAuthorityService({makeExactAuthorityMatch("live.example.com")})
  );

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("Duplicate"));
}

TEST(ResolveConfig, NoCacheAndNoServiceDefaultsCache) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.services.value().clear();
  ParsedServiceConfig svc;
  ParsedServiceConfig::MatchRule entry;
  entry.authority = AuthMatch{ParsedServiceConfig::MatchRule::AnyAuthority{true}};
  entry.path = anyPath();
  svc.match.value().push_back(std::move(entry));
  // No cache set, no service_defaults
  cfg.services.value().emplace("no-cache", std::move(svc));

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("cache.enabled is required"));
}

TEST(ResolveConfig, InvalidWildcardMissingStar) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.services.value().clear();
  cfg.services.value().emplace(
      "svc",
      makeAuthorityService({makeWildcardAuthorityMatch("example.com")})
  );

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("wildcard must start with '*.'"));
}

TEST(ResolveConfig, InvalidWildcardMultipleStars) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.services.value().clear();
  cfg.services.value().emplace(
      "svc",
      makeAuthorityService({makeWildcardAuthorityMatch("*.*.example.com")})
  );

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("exactly one '*'"));
}

TEST(ResolveConfig, DuplicateWildcardAcrossServices) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.services.value().clear();

  cfg.services.value().emplace(
      "svc1",
      makeAuthorityService({makeWildcardAuthorityMatch("*.example.com")})
  );
  cfg.services.value().emplace(
      "svc2",
      makeAuthorityService({makeWildcardAuthorityMatch("*.example.com")})
  );

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("Duplicate"));
}

TEST(ResolveConfig, ExactAuthorityEmpty) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.services.value().clear();
  cfg.services.value().emplace("svc", makeAuthorityService({makeExactAuthorityMatch("")}));

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("exact value must be non-empty"));
}

TEST(ResolveConfig, AnyAuthorityFalseRejected) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.services.value().clear();
  ParsedServiceConfig::MatchRule entry;
  entry.authority = AuthMatch{ParsedServiceConfig::MatchRule::AnyAuthority{false}};
  entry.path = anyPath();
  cfg.services.value().emplace("svc", makeAuthorityService({std::move(entry)}));

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("any must be true"));
}

TEST(ResolveConfig, DuplicateAnySamePath) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.services.value().clear();

  // Two services both with any authority and same path — duplicate
  cfg.services.value().emplace("svc1", makeAuthorityService({makeAnyAuthorityMatch()}));
  cfg.services.value().emplace("svc2", makeAuthorityService({makeAnyAuthorityMatch()}));

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("Duplicate"));
}

TEST(ResolveConfig, MultipleAnyDifferentPaths) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.services.value().clear();

  cfg.services.value().emplace(
      "svc1",
      makeAuthorityService(
          {makeAnyAuthorityMatch(PMatch{ParsedServiceConfig::MatchRule::ExactPath{"/relay"}})}
      )
  );
  cfg.services.value().emplace(
      "svc2",
      makeAuthorityService(
          {makeAnyAuthorityMatch(PMatch{ParsedServiceConfig::MatchRule::PrefixPath{"/live/"}})}
      )
  );

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
}

// --- Path validation tests ---

TEST(ResolveConfig, PathExactEmpty) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.services.value().clear();
  cfg.services.value().emplace(
      "svc",
      makeAuthorityService(
          {makeExactAuthorityMatch("a.com", PMatch{ParsedServiceConfig::MatchRule::ExactPath{""}})}
      )
  );

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("path: value must be non-empty"));
}

TEST(ResolveConfig, PathNoSlash) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.services.value().clear();
  cfg.services.value().emplace(
      "svc",
      makeAuthorityService({makeExactAuthorityMatch(
          "a.com",
          PMatch{ParsedServiceConfig::MatchRule::ExactPath{"no-slash"}}
      )})
  );

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("must start with '/'"));
}

TEST(ResolveConfig, DuplicateAuthorityPathTuple) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.services.value().clear();

  auto path = PMatch{ParsedServiceConfig::MatchRule::ExactPath{"/relay"}};
  cfg.services.value().emplace(
      "svc1",
      makeAuthorityService({makeExactAuthorityMatch("a.com", path)})
  );
  cfg.services.value().emplace(
      "svc2",
      makeAuthorityService({makeExactAuthorityMatch("a.com", path)})
  );

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("Duplicate"));
}

TEST(ResolveConfig, SameAuthorityDifferentPaths) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.services.value().clear();

  cfg.services.value().emplace(
      "svc1",
      makeAuthorityService({makeExactAuthorityMatch(
          "a.com",
          PMatch{ParsedServiceConfig::MatchRule::ExactPath{"/relay"}}
      )})
  );
  cfg.services.value().emplace(
      "svc2",
      makeAuthorityService({makeExactAuthorityMatch(
          "a.com",
          PMatch{ParsedServiceConfig::MatchRule::ExactPath{"/other"}}
      )})
  );

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
}

// --- Resolution tests ---

TEST(ResolveConfig, MinimalInsecure) {
  auto cfg = makeMinimalInsecureConfig("main");
  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  const auto& resolved = result.value().config;

  EXPECT_EQ(resolved.listener.name, "main");
  EXPECT_EQ(resolved.listener.address.getPort(), 9668);
  EXPECT_TRUE(std::holds_alternative<Insecure>(resolved.listener.tlsMode));
  EXPECT_EQ(resolved.listener.endpoint, "/moq-relay");
  EXPECT_EQ(resolved.listener.moqtVersions, "");
  EXPECT_THAT(result.value().warnings, IsEmpty());

  ASSERT_EQ(resolved.services.size(), 1);
  EXPECT_EQ(resolved.services.at("default").cache.maxCachedTracks, 100u);
  EXPECT_EQ(resolved.services.at("default").cache.maxCachedGroupsPerTrack, 3u);
}

TEST(ResolveConfig, FullTls) {
  ParsedConfig cfg;
  ParsedListenerConfig lc;
  lc.name = std::string("production");
  ParsedSocketConfig sock;
  sock.address = std::string("0.0.0.0");
  sock.port = uint16_t{4443};
  ParsedUdpConfig udp;
  udp.socket = std::move(sock);
  lc.udp = std::move(udp);
  ParsedListenerTlsConfig tls;
  tls.cert_file = std::string("/etc/ssl/cert.pem");
  tls.key_file = std::string("/etc/ssl/key.pem");
  tls.insecure = false;
  lc.tls = std::move(tls);
  lc.endpoint = std::string("/relay");
  lc.moqt_versions = std::vector<uint32_t>{14, 16};
  cfg.listeners.value().push_back(std::move(lc));
  cfg.services.value().emplace("default", makeDefaultService());
  cfg.admin = std::optional<ParsedAdminConfig>{makeDefaultAdmin()};

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  const auto& resolved = result.value().config;

  EXPECT_EQ(resolved.listener.name, "production");
  EXPECT_EQ(resolved.listener.address.getPort(), 4443);
  EXPECT_EQ(resolved.listener.endpoint, "/relay");
  EXPECT_EQ(resolved.listener.moqtVersions, "14,16");

  ASSERT_TRUE(std::holds_alternative<TlsConfig>(resolved.listener.tlsMode));
  const auto& creds = std::get<TlsConfig>(resolved.listener.tlsMode);
  EXPECT_EQ(creds.certFile, "/etc/ssl/cert.pem");
  EXPECT_EQ(creds.keyFile, "/etc/ssl/key.pem");
}

TEST(ResolveConfig, CacheDisabled) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.services.value().at("default").cache.value()->enabled = std::optional<bool>{false};

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  const auto& resolved = result.value().config;
  EXPECT_EQ(resolved.services.at("default").cache.maxCachedTracks, 0u);
  EXPECT_EQ(resolved.services.at("default").cache.maxCachedGroupsPerTrack, 3u);
}

TEST(ResolveConfig, CacheCustomValues) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.services.value().at("default").cache.value()->enabled = std::optional<bool>{true};
  cfg.services.value().at("default").cache.value()->max_tracks = std::optional<uint32_t>{200};
  cfg.services.value().at("default").cache.value()->max_groups_per_track =
      std::optional<uint32_t>{5};

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  const auto& resolved = result.value().config;
  EXPECT_EQ(resolved.services.at("default").cache.maxCachedTracks, 200u);
  EXPECT_EQ(resolved.services.at("default").cache.maxCachedGroupsPerTrack, 5u);
}

TEST(ResolveConfig, CacheInheritanceFromServiceDefaults) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.services.value().clear();

  ParsedServiceDefaultsConfig defaults;
  ParsedCacheConfig defaultCache;
  defaultCache.enabled = std::optional<bool>{true};
  defaultCache.max_tracks = std::optional<uint32_t>{50};
  defaultCache.max_groups_per_track = std::optional<uint32_t>{2};
  defaults.cache = std::move(defaultCache);
  cfg.service_defaults = std::move(defaults);

  ParsedServiceConfig svc;
  svc.match.value().push_back(makeAnyAuthorityMatch());
  cfg.services.value().emplace("inheritor", std::move(svc));

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  const auto& resolved = result.value().config;
  ASSERT_EQ(resolved.services.size(), 1);
  EXPECT_EQ(resolved.services.at("inheritor").cache.maxCachedTracks, 50u);
  EXPECT_EQ(resolved.services.at("inheritor").cache.maxCachedGroupsPerTrack, 2u);
}

TEST(ResolveConfig, CachePerServiceOverridesDefaults) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.services.value().clear();

  ParsedServiceDefaultsConfig defaults;
  ParsedCacheConfig defaultCache;
  defaultCache.enabled = std::optional<bool>{true};
  defaultCache.max_tracks = std::optional<uint32_t>{50};
  defaultCache.max_groups_per_track = std::optional<uint32_t>{2};
  defaults.cache = std::move(defaultCache);
  cfg.service_defaults = std::move(defaults);

  ParsedServiceConfig svc;
  svc.match.value().push_back(makeAnyAuthorityMatch());
  ParsedCacheConfig svcCache;
  svcCache.enabled = std::optional<bool>{true};
  svcCache.max_tracks = std::optional<uint32_t>{300};
  svcCache.max_groups_per_track = std::optional<uint32_t>{8};
  svc.cache = std::move(svcCache);
  cfg.services.value().emplace("custom", std::move(svc));

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  const auto& resolved = result.value().config;
  EXPECT_EQ(resolved.services.at("custom").cache.maxCachedTracks, 300u);
  EXPECT_EQ(resolved.services.at("custom").cache.maxCachedGroupsPerTrack, 8u);
}

TEST(ResolveConfig, CachePartialOverrideMergesWithDefaults) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.services.value().clear();

  ParsedServiceDefaultsConfig defaults;
  ParsedCacheConfig defaultCache;
  defaultCache.enabled = std::optional<bool>{true};
  defaultCache.max_tracks = std::optional<uint32_t>{50};
  defaultCache.max_groups_per_track = std::optional<uint32_t>{2};
  defaults.cache = std::move(defaultCache);
  cfg.service_defaults = std::move(defaults);

  // Service only overrides max_tracks — enabled and max_groups_per_track come from defaults.
  ParsedServiceConfig svc;
  svc.match.value().push_back(makeAnyAuthorityMatch());
  ParsedCacheConfig svcCache;
  svcCache.max_tracks = std::optional<uint32_t>{500};
  svc.cache = std::move(svcCache);
  cfg.services.value().emplace("partial", std::move(svc));

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  const auto& resolved = result.value().config;
  ASSERT_EQ(resolved.services.size(), 1);
  EXPECT_EQ(resolved.services.at("partial").cache.maxCachedTracks, 500u);
  EXPECT_EQ(resolved.services.at("partial").cache.maxCachedGroupsPerTrack, 2u);
}

TEST(ResolveConfig, CachePartialOverrideWithoutDefaultsFails) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.services.value().clear();

  // No service_defaults; service only sets max_tracks — enabled and max_groups_per_track missing.
  ParsedServiceConfig svc;
  svc.match.value().push_back(makeAnyAuthorityMatch());
  ParsedCacheConfig svcCache;
  svcCache.max_tracks = std::optional<uint32_t>{500};
  svc.cache = std::move(svcCache);
  cfg.services.value().emplace("incomplete", std::move(svc));

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("cache.enabled is required"));
  EXPECT_THAT(result.error(), HasSubstr("cache.max_groups_per_track is required"));
}

TEST(ResolveConfig, ResolveExactAuthority) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.services.value().clear();

  cfg.services.value().emplace(
      "exact-svc",
      makeAuthorityService({makeExactAuthorityMatch("live.example.com")})
  );

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  const auto& entries = result.value().config.services.at("exact-svc").match;
  ASSERT_EQ(entries.size(), 1);
  ASSERT_TRUE(std::holds_alternative<ServiceConfig::MatchEntry::ExactAuthority>(entries[0].authority
  ));
  EXPECT_EQ(
      std::get<ServiceConfig::MatchEntry::ExactAuthority>(entries[0].authority).value,
      "live.example.com"
  );
  ASSERT_TRUE(std::holds_alternative<ServiceConfig::MatchEntry::PrefixPath>(entries[0].path));
  EXPECT_EQ(std::get<ServiceConfig::MatchEntry::PrefixPath>(entries[0].path).value, "/");
}

TEST(ResolveConfig, ResolveWildcardAuthority) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.services.value().clear();

  cfg.services.value().emplace(
      "wild-svc",
      makeAuthorityService({makeWildcardAuthorityMatch("*.example.com")})
  );

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  const auto& entries = result.value().config.services.at("wild-svc").match;
  ASSERT_EQ(entries.size(), 1);
  ASSERT_TRUE(
      std::holds_alternative<ServiceConfig::MatchEntry::WildcardAuthority>(entries[0].authority)
  );
  EXPECT_EQ(
      std::get<ServiceConfig::MatchEntry::WildcardAuthority>(entries[0].authority).pattern,
      "*.example.com"
  );
}

TEST(ResolveConfig, ResolveAnyAuthority) {
  auto cfg = makeMinimalInsecureConfig();

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  const auto& match = result.value().config.services.at("default").match;
  ASSERT_EQ(match.size(), 1);
  EXPECT_TRUE(std::holds_alternative<ServiceConfig::MatchEntry::AnyAuthority>(match[0].authority));
  ASSERT_TRUE(std::holds_alternative<ServiceConfig::MatchEntry::PrefixPath>(match[0].path));
  EXPECT_EQ(std::get<ServiceConfig::MatchEntry::PrefixPath>(match[0].path).value, "/");
}

TEST(ResolveConfig, ResolveExactPath) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.services.value().clear();

  cfg.services.value().emplace(
      "svc",
      makeAuthorityService({makeExactAuthorityMatch(
          "a.com",
          PMatch{ParsedServiceConfig::MatchRule::ExactPath{"/moq-relay"}}
      )})
  );

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  const auto& entries = result.value().config.services.at("svc").match;
  ASSERT_EQ(entries.size(), 1);
  ASSERT_TRUE(std::holds_alternative<ServiceConfig::MatchEntry::ExactPath>(entries[0].path));
  EXPECT_EQ(std::get<ServiceConfig::MatchEntry::ExactPath>(entries[0].path).value, "/moq-relay");
}

TEST(ResolveConfig, ResolvePrefixPath) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.services.value().clear();

  cfg.services.value().emplace(
      "svc",
      makeAuthorityService({makeExactAuthorityMatch(
          "a.com",
          PMatch{ParsedServiceConfig::MatchRule::PrefixPath{"/live/"}}
      )})
  );

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  const auto& entries = result.value().config.services.at("svc").match;
  ASSERT_EQ(entries.size(), 1);
  ASSERT_TRUE(std::holds_alternative<ServiceConfig::MatchEntry::PrefixPath>(entries[0].path));
  EXPECT_EQ(std::get<ServiceConfig::MatchEntry::PrefixPath>(entries[0].path).value, "/live/");
}

TEST(ResolveConfig, VersionsEmpty) {
  auto cfg = makeMinimalInsecureConfig();
  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  EXPECT_EQ(result.value().config.listener.moqtVersions, "");
}

TEST(ResolveConfig, VersionsPopulated) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.listeners.value()[0].moqt_versions = std::vector<uint32_t>{14};
  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  EXPECT_EQ(result.value().config.listener.moqtVersions, "14");
}

TEST(ResolveConfig, AddressResolution) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.listeners.value()[0].udp.value().socket.value().address = std::string("127.0.0.1");
  cfg.listeners.value()[0].udp.value().socket.value().port = uint16_t{8080};

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  const auto& resolved = result.value().config;
  EXPECT_EQ(resolved.listener.address.getPort(), 8080);
  EXPECT_EQ(resolved.listener.address.getAddressStr(), "127.0.0.1");
}

// --- Admin validation tests ---

TEST(ResolveConfig, AdminPortZero) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.admin.value()->port = uint16_t{0};

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("admin.port"));
}

TEST(ResolveConfig, AdminTlsPartialCreds) {
  for (auto [cert, key] : {
           std::pair{"/cert.pem", ""},
           std::pair{"", "/key.pem"},
       }) {
    auto cfg = makeMinimalInsecureConfig();
    cfg.admin = std::optional<ParsedAdminConfig>{makeAdminWithTls(cert, key)};
    auto result = resolveConfig(cfg);
    ASSERT_TRUE(result.hasError());
    EXPECT_THAT(result.error(), HasSubstr("cert_file and key_file are required"));
  }
}

TEST(ResolveConfig, AdminPlaintextAndTlsMutuallyExclusive) {
  auto cfg = makeMinimalInsecureConfig();
  auto admin = makeAdminWithTls();
  admin.plaintext = true;
  cfg.admin = std::optional<ParsedAdminConfig>{std::move(admin)};

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("mutually exclusive"));
}

TEST(ResolveConfig, AdminNeitherPlaintextNorTls) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.admin.value()->plaintext = false;

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("plaintext or tls"));
}

// --- Admin resolution tests ---

TEST(ResolveConfig, AdminAbsent) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.admin.value() = std::nullopt;

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  EXPECT_FALSE(result.value().config.admin.has_value());
}

TEST(ResolveConfig, AdminNoTls) {
  auto cfg = makeMinimalInsecureConfig();
  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  ASSERT_TRUE(result.value().config.admin.has_value());
  EXPECT_FALSE(result.value().config.admin->tls.has_value());
}

TEST(ResolveConfig, AdminAddress) {
  auto cfg = makeMinimalInsecureConfig();
  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  ASSERT_TRUE(result.value().config.admin.has_value());
  EXPECT_EQ(result.value().config.admin->address.getPort(), 9669);
  EXPECT_EQ(result.value().config.admin->address.getAddressStr(), "::");
}

TEST(ResolveConfig, AdminCustomAddress) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.admin.value()->address = std::string("127.0.0.1");
  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  ASSERT_TRUE(result.value().config.admin.has_value());
  EXPECT_EQ(result.value().config.admin->address.getAddressStr(), "127.0.0.1");
}

TEST(ResolveConfig, AdminTlsResolution) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.admin =
      std::optional<ParsedAdminConfig>{makeAdminWithTls("/etc/ssl/cert.pem", "/etc/ssl/key.pem")};
  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  ASSERT_TRUE(result.value().config.admin.has_value());
  ASSERT_TRUE(result.value().config.admin->tls.has_value());
  const auto& tls = *result.value().config.admin->tls;
  EXPECT_EQ(tls.certFile, "/etc/ssl/cert.pem");
  EXPECT_EQ(tls.keyFile, "/etc/ssl/key.pem");
}

TEST(ResolveConfig, AdminTlsDefaultAlpn) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.admin = std::optional<ParsedAdminConfig>{makeAdminWithTls()};
  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  ASSERT_TRUE(result.value().config.admin.has_value());
  ASSERT_TRUE(result.value().config.admin->tls.has_value());
  EXPECT_THAT(result.value().config.admin->tls->alpn, ::testing::ElementsAre("h2", "http/1.1"));
}

TEST(ResolveConfig, AdminTlsCustomAlpn) {
  auto cfg = makeMinimalInsecureConfig();
  auto admin = makeAdminWithTls();
  admin.tls.value()->alpn = std::vector<std::string>{"h2"};
  cfg.admin = std::optional<ParsedAdminConfig>{std::move(admin)};
  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  ASSERT_TRUE(result.value().config.admin.has_value());
  ASSERT_TRUE(result.value().config.admin->tls.has_value());
  EXPECT_THAT(result.value().config.admin->tls->alpn, ::testing::ElementsAre("h2"));
}

// --- Upstream config tests ---

ParsedUpstreamConfig makeUpstreamConfig(
    std::string url = "moqt://origin.example.com:4433/relay",
    bool insecure = false,
    std::optional<std::string> caCert = std::nullopt
) {
  ParsedUpstreamConfig upstream;
  upstream.url = std::move(url);
  ParsedUpstreamTlsConfig tls;
  tls.insecure = insecure;
  tls.ca_cert = std::move(caCert);
  upstream.tls = std::move(tls);
  return upstream;
}

// Upstream is per-service: set it on the "default" service in the test config.
static void setServiceUpstream(ParsedConfig& cfg, ParsedUpstreamConfig upstream) {
  cfg.services.value().at("default").upstream =
      std::optional<ParsedUpstreamConfig>{std::move(upstream)};
}

TEST(ResolveConfig, UpstreamAbsent) {
  auto cfg = makeMinimalInsecureConfig();
  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  EXPECT_FALSE(result.value().config.services.at("default").upstream.has_value());
}

TEST(ResolveConfig, UpstreamInsecureFalseNoCA) {
  auto cfg = makeMinimalInsecureConfig();
  setServiceUpstream(cfg, makeUpstreamConfig());
  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  ASSERT_TRUE(result.value().config.services.at("default").upstream.has_value());
  const auto& up = *result.value().config.services.at("default").upstream;
  EXPECT_EQ(up.url, "moqt://origin.example.com:4433/relay");
  EXPECT_FALSE(up.tls.insecure);
  EXPECT_FALSE(up.tls.caCertFile.has_value());
}

TEST(ResolveConfig, UpstreamInsecureTrue) {
  auto cfg = makeMinimalInsecureConfig();
  setServiceUpstream(cfg, makeUpstreamConfig("moqt://dev:4433/", true));
  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  ASSERT_TRUE(result.value().config.services.at("default").upstream.has_value());
  EXPECT_TRUE(result.value().config.services.at("default").upstream->tls.insecure);
}

TEST(ResolveConfig, UpstreamInsecureTrueWithCACertRejected) {
  auto cfg = makeMinimalInsecureConfig();
  setServiceUpstream(cfg, makeUpstreamConfig("moqt://dev:4433/", true, "/path/to/ca.pem"));
  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("mutually exclusive"));
}

TEST(ResolveConfig, UpstreamEmptyUrlRejected) {
  auto cfg = makeMinimalInsecureConfig();
  setServiceUpstream(cfg, makeUpstreamConfig(""));
  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("upstream.url must be non-empty"));
}

// --- relayID tests ---

TEST(ResolveConfig, RelayIDAbsentGeneratesNonEmpty) {
  auto cfg = makeMinimalInsecureConfig();
  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  EXPECT_FALSE(result.value().config.relayID.empty());
}

TEST(ResolveConfig, RelayIDGeneratedUniquePerCall) {
  auto cfg = makeMinimalInsecureConfig();
  auto r1 = resolveConfig(cfg);
  auto r2 = resolveConfig(cfg);
  ASSERT_TRUE(r1.hasValue());
  ASSERT_TRUE(r2.hasValue());
  EXPECT_NE(r1.value().config.relayID, r2.value().config.relayID);
}

TEST(ResolveConfig, RelayIDExplicitPreserved) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.relay_id = std::optional<std::string>{"my-relay-1"};
  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  EXPECT_EQ(result.value().config.relayID, "my-relay-1");
}

} // namespace
} // namespace openmoq::o_rly::config
