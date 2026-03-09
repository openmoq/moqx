#include <o_rly/config/loader/config_resolver.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace openmoq::o_rly::config {
namespace {

using ::testing::HasSubstr;
using ::testing::IsEmpty;

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

// Build a minimal valid insecure listener config.
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
  ParsedTlsConfig tls;
  tls.insecure = true;
  lc.tls = std::move(tls);
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

  EXPECT_EQ(resolved.cache.maxCachedTracks, 100u);
  EXPECT_EQ(resolved.cache.maxCachedGroupsPerTrack, 3u);
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
  ParsedTlsConfig tls;
  tls.cert_file = std::string("/etc/ssl/cert.pem");
  tls.key_file = std::string("/etc/ssl/key.pem");
  tls.insecure = false;
  lc.tls = std::move(tls);
  lc.endpoint = std::string("/relay");
  lc.moqt_versions = std::vector<uint32_t>{14, 16};
  cfg.listeners.value().push_back(std::move(lc));
  cfg.cache = makeDefaultCache();
  cfg.admin = makeDefaultAdmin();

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
  cfg.cache.value().enabled = false;

  auto result = resolveConfig(cfg);
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

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  const auto& resolved = result.value().config;
  EXPECT_EQ(resolved.cache.maxCachedTracks, 200u);
  EXPECT_EQ(resolved.cache.maxCachedGroupsPerTrack, 5u);
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

} // namespace
} // namespace openmoq::o_rly::config
