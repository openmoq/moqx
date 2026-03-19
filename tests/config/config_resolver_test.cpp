#include <o_rly/config/loader/config_resolver.h>
#include <o_rly/tls/builtin_tls_providers.h>
#include <o_rly/tls/tls_cert_loader.h>
#include <o_rly/tls/tls_provider_registry.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace openmoq::o_rly::config {
namespace {

using ::testing::HasSubstr;
using ::testing::IsEmpty;

// Lightweight dummy provider — no fizz dependency needed.
class DummyCertProvider : public tls::TlsCertProvider {
public:
  folly::Expected<std::shared_ptr<const fizz::server::FizzServerContext>, std::string>
  createContext(
      const std::vector<std::string>&,
      const std::vector<tls::TicketSeed>&
  ) const override {
    return folly::makeUnexpected(std::string("dummy — not a real provider"));
  }
};

// Build a registry with real validation but dummy providers.
tls::TlsProviderRegistry makeTestRegistry() {
  tls::TlsProviderRegistry registry;
  auto dummy = [] { return std::make_shared<DummyCertProvider>(); };
  registry.registerProvider(
      "insecure",
      tls::makeInsecureFactory([dummy] { return dummy(); }));
  registry.registerProvider(
      "file",
      tls::makeFileFactory(
          [dummy](auto, auto) -> std::shared_ptr<tls::TlsCertProvider> {
            return dummy();
          }));
  registry.registerProvider(
      "directory",
      tls::makeDirectoryFactory(
          [dummy](auto, auto) -> std::shared_ptr<tls::TlsCertProvider> {
            return dummy();
          }));
  return registry;
}

ParsedCacheConfig makeDefaultCache() {
  ParsedCacheConfig cache;
  cache.enabled = true;
  cache.max_tracks = uint32_t{100};
  cache.max_groups_per_track = uint32_t{3};
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

// Build a minimal valid insecure listener config.
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
  cfg.cache = makeDefaultCache();
  cfg.admin = std::optional<ParsedAdminConfig>{makeDefaultAdmin()};
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
  cfg.cache = makeDefaultCache();
  cfg.admin = std::optional<ParsedAdminConfig>{makeDefaultAdmin()};
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
  cfg.cache = makeDefaultCache();
  cfg.admin = std::optional<ParsedAdminConfig>{makeDefaultAdmin()};
  return cfg;
}

// --- Validation error tests ---

TEST(ResolveConfig, NoListeners) {
  ParsedConfig cfg;
  cfg.cache = makeDefaultCache();
  cfg.admin = std::optional<ParsedAdminConfig>{makeDefaultAdmin()};
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

// --- Resolution tests ---

TEST(ResolveConfig, MinimalInsecure) {
  auto cfg = makeMinimalInsecureConfig("main");
  auto result = resolveConfig(cfg, makeTestRegistry());
  ASSERT_TRUE(result.hasValue());
  const auto& resolved = result.value().config;

  EXPECT_EQ(resolved.listener.name, "main");
  EXPECT_EQ(resolved.listener.address.getPort(), 9668);
  EXPECT_NE(resolved.listener.tlsProvider, nullptr);
  EXPECT_EQ(resolved.listener.endpoint, "/moq-relay");
  EXPECT_EQ(resolved.listener.moqtVersions, "");
  EXPECT_THAT(result.value().warnings, IsEmpty());

  EXPECT_EQ(resolved.cache.maxCachedTracks, 100u);
  EXPECT_EQ(resolved.cache.maxCachedGroupsPerTrack, 3u);
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
  cfg.cache.value().enabled = false;

  auto result = resolveConfig(cfg, makeTestRegistry());
  ASSERT_TRUE(result.hasValue());
  const auto& resolved = result.value().config;
  EXPECT_EQ(resolved.cache.maxCachedTracks, 0u);
  EXPECT_EQ(resolved.cache.maxCachedGroupsPerTrack, 3u);
}

TEST(ResolveConfig, CacheCustomValues) {
  auto cfg = makeMinimalInsecureConfig();
  cfg.cache.value().enabled = true;
  cfg.cache.value().max_tracks = uint32_t{200};
  cfg.cache.value().max_groups_per_track = uint32_t{5};

  auto result = resolveConfig(cfg, makeTestRegistry());
  ASSERT_TRUE(result.hasValue());
  const auto& resolved = result.value().config;
  EXPECT_EQ(resolved.cache.maxCachedTracks, 200u);
  EXPECT_EQ(resolved.cache.maxCachedGroupsPerTrack, 5u);
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
      tls::makeFileFactory(
          [](auto, auto) -> std::shared_ptr<tls::TlsCertProvider> {
            return std::make_shared<DummyCertProvider>();
          }));

  auto cfg = makeMinimalInsecureConfig();
  auto result = resolveConfig(cfg, registry);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("unknown TLS type"));
  EXPECT_THAT(result.error(), HasSubstr("insecure"));
}

TEST(ResolveConfig, RegisteredProviderAccepted) {
  // Registry with only "insecure" — insecure config should succeed
  tls::TlsProviderRegistry registry;
  registry.registerProvider(
      "insecure",
      tls::makeInsecureFactory(
          [] { return std::make_shared<DummyCertProvider>(); }));

  auto cfg = makeMinimalInsecureConfig();
  auto result = resolveConfig(cfg, registry);
  ASSERT_TRUE(result.hasValue());
  EXPECT_NE(result.value().config.listener.tlsProvider, nullptr);
}

} // namespace
} // namespace openmoq::o_rly::config
