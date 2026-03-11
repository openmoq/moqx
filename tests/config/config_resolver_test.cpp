#include <o_rly/config/loader/config_resolver.h>
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
  createContext(const std::vector<std::string>&) const override {
    return folly::makeUnexpected(std::string("dummy — not a real provider"));
  }
};

// Build a registry with dummy factories that validate required fields
// (matching the real builtin factories) but return DummyCertProvider.
tls::TlsProviderRegistry makeTestRegistry() {
  tls::TlsProviderRegistry registry;

  registry.registerProvider(
      "insecure",
      [](const ParsedTlsMode&)
          -> folly::Expected<std::shared_ptr<tls::TlsCertProvider>, std::string> {
        return std::shared_ptr<tls::TlsCertProvider>(std::make_shared<DummyCertProvider>());
      }
  );

  registry.registerProvider(
      "file",
      [](const ParsedTlsMode& tls)
          -> folly::Expected<std::shared_ptr<tls::TlsCertProvider>, std::string> {
        return tls.visit([](const auto& variant)
                             -> folly::Expected<std::shared_ptr<tls::TlsCertProvider>, std::string> {
          using T = std::decay_t<decltype(variant)>;
          if constexpr (std::is_same_v<T, ParsedTlsFile>) {
            if (variant.cert_file.value().empty()) {
              return folly::makeUnexpected(std::string("cert_file is required"));
            }
            if (variant.key_file.value().empty()) {
              return folly::makeUnexpected(std::string("key_file is required"));
            }
            return std::shared_ptr<tls::TlsCertProvider>(std::make_shared<DummyCertProvider>());
          } else {
            return folly::makeUnexpected(
                std::string("'file' factory called with wrong TLS variant")
            );
          }
        });
      }
  );

  registry.registerProvider(
      "directory",
      [](const ParsedTlsMode& tls)
          -> folly::Expected<std::shared_ptr<tls::TlsCertProvider>, std::string> {
        return tls.visit([](const auto& variant)
                             -> folly::Expected<std::shared_ptr<tls::TlsCertProvider>, std::string> {
          using T = std::decay_t<decltype(variant)>;
          if constexpr (std::is_same_v<T, ParsedTlsDirectory>) {
            if (variant.cert_dir.value().empty()) {
              return folly::makeUnexpected(std::string("cert_dir is required"));
            }
            return std::shared_ptr<tls::TlsCertProvider>(std::make_shared<DummyCertProvider>());
          } else {
            return folly::makeUnexpected(
                std::string("'directory' factory called with wrong TLS variant")
            );
          }
        });
      }
  );

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
  cfg.admin = makeDefaultAdmin();
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
  cfg.admin = makeDefaultAdmin();
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
  cfg.admin = makeDefaultAdmin();
  return cfg;
}

// --- Validation error tests ---

TEST(ResolveConfig, NoListeners) {
  ParsedConfig cfg;
  cfg.cache = makeDefaultCache();
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

} // namespace
} // namespace openmoq::o_rly::config
