#include <moqx/config/loader/config_resolver.h>
#include <moqx/tls/builtin_tls_providers.h>
#include <moqx/tls/tls_cert_loader.h>
#include <moqx/tls/tls_provider_registry.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace openmoq::moqx::config {
namespace {

using ::testing::HasSubstr;
using ::testing::IsEmpty;

using AuthMatch = ParsedServiceConfig::MatchRule::AuthorityMatch;
using PMatch = ParsedServiceConfig::MatchRule::PathMatch;

// Lightweight dummy provider — no fizz dependency needed.
class DummyCertProvider : public tls::TlsCertProvider {
public:
  folly::Expected<std::shared_ptr<const fizz::server::FizzServerContext>, std::string>
  createContext(const std::vector<std::string>&, const std::vector<tls::TicketSeed>&)
      const override {
    return folly::makeUnexpected(std::string("dummy — not a real provider"));
  }
};

// Build a registry with real validation but dummy providers.
tls::TlsProviderRegistry makeTestRegistry() {
  tls::TlsProviderRegistry registry;
  auto dummy = [] { return std::make_shared<DummyCertProvider>(); };

  registry.registerProvider("insecure", tls::makeInsecureFactory([dummy] { return dummy(); }));
  registry.registerProvider(
      "file",
      tls::makeFileFactory([dummy](auto, auto) -> std::shared_ptr<tls::TlsCertProvider> {
        return dummy();
      })
  );
  registry.registerProvider(
      "directory",
      tls::makeDirectoryFactory([dummy](auto, auto) -> std::shared_ptr<tls::TlsCertProvider> {
        return dummy();
      })
  );

  return registry;
}

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

std::optional<ParsedAdminConfig> makeDefaultAdmin() {
  ParsedAdminConfig admin;
  admin.port = uint16_t{9669};
  admin.address = std::string{"::"};
  admin.plaintext = true;
  return std::optional{admin};
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

// Helper: build a ParsedListenerConfig with the given TLS mode.
// ParsedListenerConfig can't be default-constructed (TaggedUnion has no
// default ctor), so we aggregate-initialize with a placeholder and then
// overwrite fields.
ParsedListenerConfig makeListener(ParsedTlsMode tlsMode) {
  ParsedListenerConfig lc{
      .name = std::string(""),
      .udp = ParsedUdpConfig{},
      .tls = std::move(tlsMode),
      .endpoint = std::string(""),
  };
  return lc;
}

// Build a minimal valid insecure config with one any-authority service and admin.
ParsedConfig makeMinimalInsecureConfig(std::string name = "test") {
  ParsedConfig cfg;
  auto lc = makeListener(ParsedTlsMode{ParsedTlsInsecure{}});
  lc.name = std::move(name);
  ParsedSocketConfig sock;
  sock.address = std::string("::");
  sock.port = uint16_t{9668};
  ParsedUdpConfig udp;
  udp.socket = std::move(sock);
  lc.udp = std::move(udp);
  lc.endpoint = std::string("/moq-relay");
  cfg.listeners.value().push_back(std::move(lc));
  cfg.admin = makeDefaultAdmin();
  cfg.services.value().emplace("default", makeDefaultService());
  return cfg;
}

ParsedConfig makeFileConfig(
    std::string certFile = "/etc/ssl/cert.pem",
    std::string keyFile = "/etc/ssl/key.pem"
) {
  ParsedConfig cfg;
  ParsedTlsFile tls;
  tls.cert_file = std::move(certFile);
  tls.key_file = std::move(keyFile);
  auto lc = makeListener(ParsedTlsMode{std::move(tls)});
  lc.name = std::string("production");
  ParsedSocketConfig sock;
  sock.address = std::string("0.0.0.0");
  sock.port = uint16_t{4443};
  ParsedUdpConfig udp;
  udp.socket = std::move(sock);
  lc.udp = std::move(udp);
  lc.endpoint = std::string("/relay");
  lc.moqt_versions = std::vector<uint32_t>{14, 16};
  cfg.listeners.value().push_back(std::move(lc));
  cfg.admin = makeDefaultAdmin();
  cfg.services.value().emplace("default", makeDefaultService());
  return cfg;
}

ParsedConfig
makeDirectoryConfig(std::string certDir = "/etc/ssl/certs.d/", std::string defaultCert = "") {
  ParsedConfig cfg;
  ParsedTlsDirectory tls;
  tls.cert_dir = std::move(certDir);
  if (!defaultCert.empty()) {
    tls.default_cert = std::move(defaultCert);
  }
  auto lc = makeListener(ParsedTlsMode{std::move(tls)});
  lc.name = std::string("multi");
  ParsedSocketConfig sock;
  sock.address = std::string("::");
  sock.port = uint16_t{4443};
  ParsedUdpConfig udp;
  udp.socket = std::move(sock);
  lc.udp = std::move(udp);
  lc.endpoint = std::string("/moq-relay");
  cfg.listeners.value().push_back(std::move(lc));
  cfg.services.value().emplace("default", makeDefaultService());
  cfg.admin = makeDefaultAdmin();
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
  cfg.admin = makeDefaultAdmin();
  auto result = resolveConfig(cfg, makeTestRegistry());
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("At least one listener"));
}

TEST(ResolveConfig, FileTypeEmptyCertFile) {
  auto cfg = makeFileConfig("", "/etc/ssl/key.pem");
  auto result = resolveConfig(cfg, makeTestRegistry());
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("cert_file"));
}

TEST(ResolveConfig, FileTypeEmptyKeyFile) {
  auto cfg = makeFileConfig("/etc/ssl/cert.pem", "");
  auto result = resolveConfig(cfg, makeTestRegistry());
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("key_file"));
}

TEST(ResolveConfig, DirectoryTypeEmptyCertDir) {
  auto cfg = makeDirectoryConfig("");
  auto result = resolveConfig(cfg, makeTestRegistry());
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("cert_dir"));
}

TEST(ResolveConfig, PortZero) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.listeners.value()[0].udp.value().socket.value().port = uint16_t{0};

  auto result = resolveConfig(cfg, makeTestRegistry());
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("port"));
}

// --- Service validation error tests ---

TEST(ResolveConfig, NoServices) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.services.value().clear();

  auto result = resolveConfig(cfg, makeTestRegistry());
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

  auto result = resolveConfig(cfg, makeTestRegistry());
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

  auto result = resolveConfig(cfg, makeTestRegistry());
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

  auto result = resolveConfig(cfg, makeTestRegistry());
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

  auto result = resolveConfig(cfg, makeTestRegistry());
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

  auto result = resolveConfig(cfg, makeTestRegistry());
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("Duplicate"));
}

TEST(ResolveConfig, ExactAuthorityEmpty) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.services.value().clear();
  cfg.services.value().emplace("svc", makeAuthorityService({makeExactAuthorityMatch("")}));

  auto result = resolveConfig(cfg, makeTestRegistry());
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

  auto result = resolveConfig(cfg, makeTestRegistry());
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("any must be true"));
}

TEST(ResolveConfig, DuplicateAnySamePath) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.services.value().clear();

  // Two services both with any authority and same path — duplicate
  cfg.services.value().emplace("svc1", makeAuthorityService({makeAnyAuthorityMatch()}));
  cfg.services.value().emplace("svc2", makeAuthorityService({makeAnyAuthorityMatch()}));

  auto result = resolveConfig(cfg, makeTestRegistry());
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

  auto result = resolveConfig(cfg, makeTestRegistry());
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

  auto result = resolveConfig(cfg, makeTestRegistry());
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

  auto result = resolveConfig(cfg, makeTestRegistry());
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

  auto result = resolveConfig(cfg, makeTestRegistry());
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

  auto result = resolveConfig(cfg, makeTestRegistry());
  ASSERT_TRUE(result.hasValue());
}

// --- Resolution tests ---

TEST(ResolveConfig, MinimalInsecure) {
  auto cfg = makeMinimalInsecureConfig("main");
  auto result = resolveConfig(cfg, makeTestRegistry());
  ASSERT_TRUE(result.hasValue()) << result.error();
  const auto& resolved = result.value().config;

  EXPECT_EQ(resolved.listener.name, "main");
  EXPECT_EQ(resolved.listener.address.getPort(), 9668);
  EXPECT_NE(resolved.listener.tlsProvider, nullptr);
  EXPECT_EQ(resolved.listener.endpoint, "/moq-relay");
  EXPECT_EQ(resolved.listener.moqtVersions, "");
  EXPECT_THAT(result.value().warnings, IsEmpty());

  ASSERT_EQ(resolved.services.size(), 1);
  EXPECT_EQ(resolved.services.at("default").cache.maxCachedTracks, 100u);
  EXPECT_EQ(resolved.services.at("default").cache.maxCachedGroupsPerTrack, 3u);
}

TEST(ResolveConfig, FullTlsFile) {
  auto cfg = makeFileConfig();
  auto result = resolveConfig(cfg, makeTestRegistry());
  ASSERT_TRUE(result.hasValue());
  const auto& resolved = result.value().config;

  EXPECT_EQ(resolved.listener.name, "production");
  EXPECT_EQ(resolved.listener.address.getPort(), 4443);
  EXPECT_EQ(resolved.listener.endpoint, "/relay");
  EXPECT_EQ(resolved.listener.moqtVersions, "14,16");
  EXPECT_NE(resolved.listener.tlsProvider, nullptr);
}

TEST(ResolveConfig, TlsDirectory) {
  auto cfg = makeDirectoryConfig("/etc/ssl/certs.d/", "example.com");
  auto result = resolveConfig(cfg, makeTestRegistry());
  ASSERT_TRUE(result.hasValue());
  const auto& resolved = result.value().config;
  EXPECT_NE(resolved.listener.tlsProvider, nullptr);
}

TEST(ResolveConfig, TlsDirectoryNoDefault) {
  auto cfg = makeDirectoryConfig("/etc/ssl/certs.d/");
  auto result = resolveConfig(cfg, makeTestRegistry());
  ASSERT_TRUE(result.hasValue());
  const auto& resolved = result.value().config;
  EXPECT_NE(resolved.listener.tlsProvider, nullptr);
}

TEST(ResolveConfig, CacheDisabled) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.services.value().at("default").cache.value()->enabled = std::optional<bool>{false};

  auto result = resolveConfig(cfg, makeTestRegistry());
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

  auto result = resolveConfig(cfg, makeTestRegistry());
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

  auto result = resolveConfig(cfg, makeTestRegistry());
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

  auto result = resolveConfig(cfg, makeTestRegistry());
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

  auto result = resolveConfig(cfg, makeTestRegistry());
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

  auto result = resolveConfig(cfg, makeTestRegistry());
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

  auto result = resolveConfig(cfg, makeTestRegistry());
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

  auto result = resolveConfig(cfg, makeTestRegistry());
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

  auto result = resolveConfig(cfg, makeTestRegistry());
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

  auto result = resolveConfig(cfg, makeTestRegistry());
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

  auto result = resolveConfig(cfg, makeTestRegistry());
  ASSERT_TRUE(result.hasValue());
  const auto& entries = result.value().config.services.at("svc").match;
  ASSERT_EQ(entries.size(), 1);
  ASSERT_TRUE(std::holds_alternative<ServiceConfig::MatchEntry::PrefixPath>(entries[0].path));
  EXPECT_EQ(std::get<ServiceConfig::MatchEntry::PrefixPath>(entries[0].path).value, "/live/");
}

TEST(ResolveConfig, VersionsEmpty) {
  auto cfg = makeMinimalInsecureConfig();
  auto result = resolveConfig(cfg, makeTestRegistry());
  ASSERT_TRUE(result.hasValue());
  EXPECT_EQ(result.value().config.listener.moqtVersions, "");
}

TEST(ResolveConfig, VersionsPopulated) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.listeners.value()[0].moqt_versions = std::vector<uint32_t>{14};
  auto result = resolveConfig(cfg, makeTestRegistry());
  ASSERT_TRUE(result.hasValue());
  EXPECT_EQ(result.value().config.listener.moqtVersions, "14");
}

TEST(ResolveConfig, AddressResolution) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.listeners.value()[0].udp.value().socket.value().address = std::string("127.0.0.1");
  cfg.listeners.value()[0].udp.value().socket.value().port = uint16_t{8080};

  auto result = resolveConfig(cfg, makeTestRegistry());
  ASSERT_TRUE(result.hasValue());
  const auto& resolved = result.value().config;
  EXPECT_EQ(resolved.listener.address.getPort(), 8080);
  EXPECT_EQ(resolved.listener.address.getAddressStr(), "127.0.0.1");
}

// --- Admin validation tests ---

TEST(ResolveConfig, AdminPortZero) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.admin.value()->port = uint16_t{0};

  auto result = resolveConfig(cfg, makeTestRegistry());
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
    auto result = resolveConfig(cfg, makeTestRegistry());
    ASSERT_TRUE(result.hasError());
    EXPECT_THAT(result.error(), HasSubstr("cert_file and key_file are required"));
  }
}

TEST(ResolveConfig, AdminPlaintextAndTlsMutuallyExclusive) {
  auto cfg = makeMinimalInsecureConfig();
  auto admin = makeAdminWithTls();
  admin.plaintext = true;
  cfg.admin = std::optional<ParsedAdminConfig>{std::move(admin)};

  auto result = resolveConfig(cfg, makeTestRegistry());
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("mutually exclusive"));
}

TEST(ResolveConfig, AdminNeitherPlaintextNorTls) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.admin.value()->plaintext = false;

  auto result = resolveConfig(cfg, makeTestRegistry());
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("plaintext or tls"));
}

// --- Admin resolution tests ---

TEST(ResolveConfig, AdminAbsent) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.admin.value() = std::nullopt;

  auto result = resolveConfig(cfg, makeTestRegistry());
  ASSERT_TRUE(result.hasValue());
  EXPECT_FALSE(result.value().config.admin.has_value());
}

TEST(ResolveConfig, AdminNoTls) {
  auto cfg = makeMinimalInsecureConfig();
  auto result = resolveConfig(cfg, makeTestRegistry());
  ASSERT_TRUE(result.hasValue());
  ASSERT_TRUE(result.value().config.admin.has_value());
  EXPECT_FALSE(result.value().config.admin->tls.has_value());
}

TEST(ResolveConfig, AdminAddress) {
  auto cfg = makeMinimalInsecureConfig();
  auto result = resolveConfig(cfg, makeTestRegistry());
  ASSERT_TRUE(result.hasValue());
  ASSERT_TRUE(result.value().config.admin.has_value());
  EXPECT_EQ(result.value().config.admin->address.getPort(), 9669);
  EXPECT_EQ(result.value().config.admin->address.getAddressStr(), "::");
}

TEST(ResolveConfig, AdminCustomAddress) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.admin.value()->address = std::string("127.0.0.1");
  auto result = resolveConfig(cfg, makeTestRegistry());
  ASSERT_TRUE(result.hasValue());
  ASSERT_TRUE(result.value().config.admin.has_value());
  EXPECT_EQ(result.value().config.admin->address.getAddressStr(), "127.0.0.1");
}

TEST(ResolveConfig, AdminTlsResolution) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.admin =
      std::optional<ParsedAdminConfig>{makeAdminWithTls("/etc/ssl/cert.pem", "/etc/ssl/key.pem")};
  auto result = resolveConfig(cfg, makeTestRegistry());
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
  auto result = resolveConfig(cfg, makeTestRegistry());
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
  auto result = resolveConfig(cfg, makeTestRegistry());
  ASSERT_TRUE(result.hasValue());
  ASSERT_TRUE(result.value().config.admin.has_value());
  ASSERT_TRUE(result.value().config.admin->tls.has_value());
  EXPECT_THAT(result.value().config.admin->tls->alpn, ::testing::ElementsAre("h2"));
}

// --- Partial registration tests ---

TEST(ResolveConfig, UnregisteredProviderRejected) {
  // Registry with only "file" — insecure config should fail
  tls::TlsProviderRegistry registry;
  registry.registerProvider(
      "file",
      tls::makeFileFactory([](auto, auto) -> std::shared_ptr<tls::TlsCertProvider> {
        return std::make_shared<DummyCertProvider>();
      })
  );

  auto cfg = makeMinimalInsecureConfig();
  auto result = resolveConfig(cfg, registry);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("unknown TLS type"));
  EXPECT_THAT(result.error(), HasSubstr("insecure"));
}

TEST(ResolveConfig, RegisteredProviderAccepted) {
  // Registry with only "insecure" — insecure config should succeed
  tls::TlsProviderRegistry registry;
  registry.registerProvider("insecure", tls::makeInsecureFactory([] {
                              return std::make_shared<DummyCertProvider>();
                            }));

  auto cfg = makeMinimalInsecureConfig();
  auto result = resolveConfig(cfg, registry);
  ASSERT_TRUE(result.hasValue());
  EXPECT_NE(result.value().config.listener.tlsProvider, nullptr);
}

} // namespace
} // namespace openmoq::moqx::config
