#include "config/loader/config_resolver.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace openmoq::moqx::config {
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

TEST(ResolveConfig, PicoquicInsecureRejected) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.listeners.value()[0].quic_stack = std::string("picoquic");

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("insecure: true is not supported"));
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

  EXPECT_EQ(resolved.listeners[0].name, "main");
  EXPECT_EQ(resolved.listeners[0].address.getPort(), 9668);
  EXPECT_TRUE(std::holds_alternative<Insecure>(resolved.listeners[0].tlsMode));
  EXPECT_EQ(resolved.listeners[0].endpoint, "/moq-relay");
  EXPECT_EQ(resolved.listeners[0].moqtVersions, "");
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

  EXPECT_EQ(resolved.listeners[0].name, "production");
  EXPECT_EQ(resolved.listeners[0].address.getPort(), 4443);
  EXPECT_EQ(resolved.listeners[0].endpoint, "/relay");
  EXPECT_EQ(resolved.listeners[0].moqtVersions, "14,16");

  ASSERT_TRUE(std::holds_alternative<TlsConfig>(resolved.listeners[0].tlsMode));
  const auto& creds = std::get<TlsConfig>(resolved.listeners[0].tlsMode);
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
  EXPECT_EQ(result.value().config.listeners[0].moqtVersions, "");
}

TEST(ResolveConfig, VersionsPopulated) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.listeners.value()[0].moqt_versions = std::vector<uint32_t>{14};
  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  EXPECT_EQ(result.value().config.listeners[0].moqtVersions, "14");
}

TEST(ResolveConfig, AddressResolution) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.listeners.value()[0].udp.value().socket.value().address = std::string("127.0.0.1");
  cfg.listeners.value()[0].udp.value().socket.value().port = uint16_t{8080};

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  const auto& resolved = result.value().config;
  EXPECT_EQ(resolved.listeners[0].address.getPort(), 8080);
  EXPECT_EQ(resolved.listeners[0].address.getAddressStr(), "127.0.0.1");
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

// --- threads tests ---

TEST(ResolveConfig, ThreadsAbsentDefaultsToOne) {
  auto cfg = makeMinimalInsecureConfig();
  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  EXPECT_EQ(result.value().config.threads, 1u);
}

TEST(ResolveConfig, ThreadsExplicitOneAccepted) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.threads = std::optional<uint32_t>{1};
  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  EXPECT_EQ(result.value().config.threads, 1u);
}

TEST(ResolveConfig, ThreadsZeroRejected) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.threads = std::optional<uint32_t>{0};
  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("threads must be >= 1"));
}

TEST(ResolveConfig, ThreadsGreaterThanOneRejected) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.threads = std::optional<uint32_t>{2};
  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("threads > 1 is not yet supported"));
}

// --- multiple listeners tests ---

TEST(ResolveConfig, MultipleListeners) {
  auto cfg = makeMinimalInsecureConfig("first");

  ParsedListenerConfig lc2;
  lc2.name = std::string("second");
  ParsedSocketConfig sock2;
  sock2.address = std::string("::");
  sock2.port = uint16_t{9669};
  ParsedUdpConfig udp2;
  udp2.socket = std::move(sock2);
  lc2.udp = std::move(udp2);
  ParsedListenerTlsConfig tls2;
  tls2.insecure = true;
  lc2.tls = std::move(tls2);
  lc2.endpoint = std::string("/moq-relay");
  cfg.listeners.value().push_back(std::move(lc2));

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  ASSERT_EQ(result.value().config.listeners.size(), 2u);
  EXPECT_EQ(result.value().config.listeners[0].name, "first");
  EXPECT_EQ(result.value().config.listeners[0].address.getPort(), 9668);
  EXPECT_EQ(result.value().config.listeners[1].name, "second");
  EXPECT_EQ(result.value().config.listeners[1].address.getPort(), 9669);
}

TEST(ResolveConfig, MultipleListenersDuplicateAddress) {
  auto cfg = makeMinimalInsecureConfig("first");

  ParsedListenerConfig lc2;
  lc2.name = std::string("second");
  lc2.udp = cfg.listeners.value()[0].udp; // same address as first
  ParsedListenerTlsConfig tls2;
  tls2.insecure = true;
  lc2.tls = std::move(tls2);
  lc2.endpoint = std::string("/moq-relay");
  cfg.listeners.value().push_back(std::move(lc2));

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("Duplicate listener address"));
}

TEST(ResolveConfig, MultipleListenersInvalidPort) {
  auto cfg = makeMinimalInsecureConfig("first");

  ParsedListenerConfig lc2;
  lc2.name = std::string("bad");
  ParsedSocketConfig sock2;
  sock2.address = std::string("::");
  sock2.port = uint16_t{0};
  ParsedUdpConfig udp2;
  udp2.socket = std::move(sock2);
  lc2.udp = std::move(udp2);
  ParsedListenerTlsConfig tls2;
  tls2.insecure = true;
  lc2.tls = std::move(tls2);
  lc2.endpoint = std::string("/moq-relay");
  cfg.listeners.value().push_back(std::move(lc2));

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("bad"));
  EXPECT_THAT(result.error(), HasSubstr("port"));
}

// --- QuicConfig resolution tests ---

TEST(ResolveConfig, QuicDefaultsUsedWhenNoneSpecified) {
  auto cfg = makeMinimalInsecureConfig();
  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  const auto& quic = result.value().config.listeners[0].quic;
  EXPECT_EQ(quic.maxData, 67108864u);
  EXPECT_EQ(quic.maxStreamData, 16777216u);
  EXPECT_EQ(quic.maxUniStreams, 8192u);
  EXPECT_EQ(quic.maxBidiStreams, 16u);
  EXPECT_EQ(quic.idleTimeoutMs, 30000u);
  EXPECT_EQ(quic.maxAckDelayUs, 25000u);
  EXPECT_EQ(quic.minAckDelayUs, 1000u);
  EXPECT_EQ(quic.defaultStreamPriority, 2u);
  EXPECT_EQ(quic.defaultDatagramPriority, 1u);
  EXPECT_EQ(quic.ccAlgo, "bbr");
}

TEST(ResolveConfig, ListenerDefaultsQuicApplied) {
  auto cfg = makeMinimalInsecureConfig();
  ParsedQuicConfig quicCfg;
  quicCfg.max_data = std::optional<uint64_t>{33554432};
  quicCfg.max_uni_streams = std::optional<uint64_t>{4096};
  ParsedListenerDefaultsConfig ld;
  ld.quic = std::optional<ParsedQuicConfig>{std::move(quicCfg)};
  cfg.listener_defaults = std::optional<ParsedListenerDefaultsConfig>{std::move(ld)};

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  const auto& quic = result.value().config.listeners[0].quic;
  EXPECT_EQ(quic.maxData, 33554432u);
  EXPECT_EQ(quic.maxStreamData, 16777216u); // unchanged default
  EXPECT_EQ(quic.maxUniStreams, 4096u);
  EXPECT_EQ(quic.maxBidiStreams, 16u); // unchanged default
}

TEST(ResolveConfig, PerListenerQuicOverridesDefaults) {
  auto cfg = makeMinimalInsecureConfig();

  // Set listener_defaults
  ParsedQuicConfig defaultQuic;
  defaultQuic.max_data = std::optional<uint64_t>{33554432};
  defaultQuic.max_uni_streams = std::optional<uint64_t>{4096};
  ParsedListenerDefaultsConfig ld;
  ld.quic = std::optional<ParsedQuicConfig>{std::move(defaultQuic)};
  cfg.listener_defaults = std::optional<ParsedListenerDefaultsConfig>{std::move(ld)};

  // Set per-listener override for max_data only
  ParsedQuicConfig perListenerQuic;
  perListenerQuic.max_data = std::optional<uint64_t>{67108864};
  cfg.listeners.value()[0].quic = std::optional<ParsedQuicConfig>{std::move(perListenerQuic)};

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  const auto& quic = result.value().config.listeners[0].quic;
  EXPECT_EQ(quic.maxData, 67108864u);   // per-listener wins
  EXPECT_EQ(quic.maxUniStreams, 4096u); // from listener_defaults
  EXPECT_EQ(quic.maxBidiStreams, 16u);  // struct default
}

TEST(ResolveConfig, PerListenerQuicWithNoDefaults) {
  auto cfg = makeMinimalInsecureConfig();
  ParsedQuicConfig perListenerQuic;
  perListenerQuic.max_bidi_streams = std::optional<uint64_t>{512};
  cfg.listeners.value()[0].quic = std::optional<ParsedQuicConfig>{std::move(perListenerQuic)};

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  const auto& quic = result.value().config.listeners[0].quic;
  EXPECT_EQ(quic.maxBidiStreams, 512u);
  EXPECT_EQ(quic.maxData, 67108864u); // struct default
}

TEST(ResolveConfig, QuicConnFcLessThanStreamFcIsError) {
  auto cfg = makeMinimalInsecureConfig();
  ParsedQuicConfig quicCfg;
  quicCfg.max_data = std::optional<uint64_t>{1000};
  quicCfg.max_stream_data = std::optional<uint64_t>{2000};
  cfg.listeners.value()[0].quic = std::optional<ParsedQuicConfig>{std::move(quicCfg)};

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("max_data"));
  EXPECT_THAT(result.error(), HasSubstr("max_stream_data"));
}

TEST(ResolveConfig, QuicLowUniStreamsWarning) {
  auto cfg = makeMinimalInsecureConfig();
  ParsedQuicConfig quicCfg;
  quicCfg.max_uni_streams = std::optional<uint64_t>{10};
  cfg.listeners.value()[0].quic = std::optional<ParsedQuicConfig>{std::move(quicCfg)};

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  ASSERT_FALSE(result.value().warnings.empty());
  EXPECT_THAT(result.value().warnings[0], HasSubstr("max_uni_streams"));
}

TEST(ResolveConfig, QuicLowBidiStreamsWarning) {
  auto cfg = makeMinimalInsecureConfig();
  ParsedQuicConfig quicCfg;
  quicCfg.max_bidi_streams = std::optional<uint64_t>{5};
  cfg.listeners.value()[0].quic = std::optional<ParsedQuicConfig>{std::move(quicCfg)};

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  ASSERT_FALSE(result.value().warnings.empty());
  EXPECT_THAT(result.value().warnings[0], HasSubstr("max_bidi_streams"));
}

TEST(ResolveConfig, QuicValidationUseMergedValues) {
  // max_data set in listener_defaults, max_stream_data overridden per-listener to exceed it
  auto cfg = makeMinimalInsecureConfig();
  ParsedQuicConfig defaults;
  defaults.max_data = std::optional<uint64_t>{4096};
  ParsedListenerDefaultsConfig ld;
  ld.quic = std::optional<ParsedQuicConfig>{std::move(defaults)};
  cfg.listener_defaults = std::optional<ParsedListenerDefaultsConfig>{std::move(ld)};

  ParsedQuicConfig perListener;
  perListener.max_stream_data = std::optional<uint64_t>{8192}; // exceeds max_data from defaults
  cfg.listeners.value()[0].quic = std::optional<ParsedQuicConfig>{std::move(perListener)};

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("max_data"));
}

TEST(ResolveConfig, QuicIdleTimeoutLowWarning) {
  auto cfg = makeMinimalInsecureConfig();
  ParsedQuicConfig quicCfg;
  quicCfg.idle_timeout_ms = std::optional<uint64_t>{1000};
  cfg.listeners.value()[0].quic = std::optional<ParsedQuicConfig>{std::move(quicCfg)};

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  ASSERT_FALSE(result.value().warnings.empty());
  EXPECT_THAT(result.value().warnings[0], HasSubstr("idle_timeout_ms"));
}

TEST(ResolveConfig, QuicMaxAckDelayLessThanMinIsError) {
  auto cfg = makeMinimalInsecureConfig();
  ParsedQuicConfig quicCfg;
  quicCfg.max_ack_delay_us = std::optional<uint32_t>{500};
  quicCfg.min_ack_delay_us = std::optional<uint32_t>{1000};
  cfg.listeners.value()[0].quic = std::optional<ParsedQuicConfig>{std::move(quicCfg)};

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("max_ack_delay_us"));
  EXPECT_THAT(result.error(), HasSubstr("min_ack_delay_us"));
}

TEST(ResolveConfig, QuicUnknownCcAlgoIsError) {
  auto cfg = makeMinimalInsecureConfig();
  ParsedQuicConfig quicCfg;
  quicCfg.cc_algo = std::optional<std::string>{"notanalgo"};
  cfg.listeners.value()[0].quic = std::optional<ParsedQuicConfig>{std::move(quicCfg)};

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("cc_algo"));
  EXPECT_THAT(result.error(), HasSubstr("notanalgo"));
}

TEST(ResolveConfig, QuicCcAlgoPicoOnlyRejectedByMvfst) {
  // dcubic is pico-only; rejected on mvfst (default stack), accepted on picoquic
  auto cfg = makeMinimalInsecureConfig();
  ParsedQuicConfig quicCfg;
  quicCfg.cc_algo = std::optional<std::string>{"dcubic"};
  cfg.listeners.value()[0].quic = std::optional<ParsedQuicConfig>{quicCfg};

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("cc_algo"));
  EXPECT_THAT(result.error(), HasSubstr("dcubic"));

  cfg.listeners.value()[0].quic_stack = std::optional<std::string>{"picoquic"};
  cfg.listeners.value()[0].tls.value().insecure = false;
  cfg.listeners.value()[0].tls.value().cert_file = std::optional<std::string>{"cert.pem"};
  cfg.listeners.value()[0].tls.value().key_file = std::optional<std::string>{"key.pem"};
  auto result2 = resolveConfig(cfg);
  ASSERT_TRUE(result2.hasValue());
  EXPECT_EQ(result2.value().config.listeners[0].quic.ccAlgo, "dcubic");
}

TEST(ResolveConfig, QuicCcAlgoMvfstOnlyRejectedByPico) {
  // bbr2 is mvfst-only; rejected on picoquic, accepted on mvfst (default stack)
  auto cfg = makeMinimalInsecureConfig();
  ParsedQuicConfig quicCfg;
  quicCfg.cc_algo = std::optional<std::string>{"bbr2"};
  cfg.listeners.value()[0].quic = std::optional<ParsedQuicConfig>{quicCfg};

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  EXPECT_EQ(result.value().config.listeners[0].quic.ccAlgo, "bbr2");

  cfg.listeners.value()[0].quic_stack = std::optional<std::string>{"picoquic"};
  cfg.listeners.value()[0].tls.value().insecure = false;
  cfg.listeners.value()[0].tls.value().cert_file = std::optional<std::string>{"cert.pem"};
  cfg.listeners.value()[0].tls.value().key_file = std::optional<std::string>{"key.pem"};
  cfg.listeners.value()[0].quic = std::optional<ParsedQuicConfig>{quicCfg};
  auto result2 = resolveConfig(cfg);
  ASSERT_TRUE(result2.hasError());
  EXPECT_THAT(result2.error(), HasSubstr("cc_algo"));
  EXPECT_THAT(result2.error(), HasSubstr("bbr2"));
}

TEST(ResolveConfig, QuicCcAlgoRoundTrips) {
  auto cfg = makeMinimalInsecureConfig();
  ParsedQuicConfig quicCfg;
  quicCfg.cc_algo = std::optional<std::string>{"cubic"};
  cfg.listeners.value()[0].quic = std::optional<ParsedQuicConfig>{std::move(quicCfg)};

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  EXPECT_EQ(result.value().config.listeners[0].quic.ccAlgo, "cubic");
}

// --- Pico WebTransport path validation tests ---

// Helper: make a minimal pico listener config (TLS required for picoquic).
ParsedConfig makeMinimalPicoConfig() {
  auto cfg = makeMinimalInsecureConfig();
  auto& lc = cfg.listeners.value()[0];
  lc.quic_stack = std::optional<std::string>{"picoquic"};
  lc.tls.value().insecure = false;
  lc.tls.value().cert_file = std::optional<std::string>{"cert.pem"};
  lc.tls.value().key_file = std::optional<std::string>{"key.pem"};
  return cfg;
}

TEST(ResolveConfig, PicoPrefixPathServiceWarning) {
  // The default service uses only a prefix path — pico warns about the prefix
  // rule AND about having no exact-path endpoints registered.
  auto cfg = makeMinimalPicoConfig();
  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  const auto& warnings = result.value().warnings;
  EXPECT_THAT(warnings, ::testing::Contains(HasSubstr("prefix path")));
  EXPECT_THAT(warnings, ::testing::Contains(HasSubstr("no WebTransport endpoints")));
}

TEST(ResolveConfig, PicoNoExactPathsWarning) {
  // A pico listener with no services at all that have exact paths should warn
  // that no WT endpoints will be registered.
  auto cfg = makeMinimalPicoConfig();
  // Replace default service with one that has only a prefix path.
  cfg.services.value().clear();
  ParsedServiceConfig svc;
  ParsedServiceConfig::MatchRule entry;
  entry.authority = AuthMatch{ParsedServiceConfig::MatchRule::AnyAuthority{true}};
  entry.path = PMatch{ParsedServiceConfig::MatchRule::PrefixPath{"/"}};
  svc.match.value().push_back(std::move(entry));
  svc.cache = makeDefaultCache();
  cfg.services.value().emplace("prefix-only-svc", std::move(svc));

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  EXPECT_THAT(result.value().warnings, ::testing::Contains(HasSubstr("no WebTransport endpoints")));
}

TEST(ResolveConfig, PicoExactPathServiceNoWarning) {
  auto cfg = makeMinimalPicoConfig();
  // Replace the default prefix-path service with an exact-path one.
  cfg.services.value().clear();
  ParsedServiceConfig svc;
  ParsedServiceConfig::MatchRule entry;
  entry.authority = AuthMatch{ParsedServiceConfig::MatchRule::AnyAuthority{true}};
  entry.path = PMatch{ParsedServiceConfig::MatchRule::ExactPath{"/moq-relay"}};
  svc.match.value().push_back(std::move(entry));
  svc.cache = makeDefaultCache();
  cfg.services.value().emplace("exact-svc", std::move(svc));

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  // No pico path warnings
  for (const auto& w : result.value().warnings) {
    EXPECT_THAT(w, ::testing::Not(HasSubstr("Picoquic listener")));
  }
}

TEST(ResolveConfig, MvfstPrefixPathServiceNoWarning) {
  // Prefix paths on mvfst listeners are fine — no pico warning.
  auto cfg = makeMinimalInsecureConfig(); // default stack = mvfst
  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  for (const auto& w : result.value().warnings) {
    EXPECT_THAT(w, ::testing::Not(HasSubstr("Picoquic listener")));
  }
}

} // namespace
} // namespace openmoq::moqx::config
