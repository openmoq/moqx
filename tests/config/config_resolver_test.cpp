#include <o_rly/config/loader/config_resolver.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace openmoq::o_rly::config {
namespace {

using ::testing::HasSubstr;
using ::testing::IsEmpty;

// Build a minimal valid insecure listener config (name + udp defaults + tls insecure).
ParsedConfig makeMinimalInsecureConfig(std::string name = "test") {
  ParsedConfig cfg;
  ParsedListenerConfig lc;
  lc.name = std::move(name);
  lc.udp = ParsedUdpConfig{};
  lc.tls = ParsedTlsConfig{};
  lc.tls.value()->insecure = true;
  cfg.listeners.value().push_back(std::move(lc));
  return cfg;
}

// --- Validation error tests ---

TEST(ResolveConfig, NoListeners) {
  ParsedConfig cfg;
  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("At least one listener"));
}

TEST(ResolveConfig, MissingUdp) {
  ParsedConfig cfg;
  ParsedListenerConfig lc;
  lc.name = "test";
  lc.tls = ParsedTlsConfig{};
  lc.tls.value()->insecure = true;
  cfg.listeners.value().push_back(std::move(lc));

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("udp"));
}

TEST(ResolveConfig, InsecureFalseNoCerts) {
  ParsedConfig cfg;
  ParsedListenerConfig lc;
  lc.name = "test";
  lc.udp = ParsedUdpConfig{};
  lc.tls = ParsedTlsConfig{};
  // insecure defaults to false, no cert/key set
  cfg.listeners.value().push_back(std::move(lc));

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("cert_file"));
}

TEST(ResolveConfig, PortZero) {
  ParsedConfig cfg;
  ParsedListenerConfig lc;
  lc.name = "test";
  ParsedUdpConfig udp;
  ParsedSocketConfig sock;
  sock.port = static_cast<uint16_t>(0);
  udp.socket = std::move(sock);
  lc.udp = std::move(udp);
  lc.tls = ParsedTlsConfig{};
  lc.tls.value()->insecure = true;
  cfg.listeners.value().push_back(std::move(lc));

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("port"));
}

TEST(ResolveConfig, NoTlsConfig) {
  ParsedConfig cfg;
  ParsedListenerConfig lc;
  lc.name = "test";
  lc.udp = ParsedUdpConfig{};
  // No tls set at all
  cfg.listeners.value().push_back(std::move(lc));

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("tls"));
}

TEST(ResolveConfig, InsecureWithCertsWarning) {
  ParsedConfig cfg;
  ParsedListenerConfig lc;
  lc.name = "test";
  lc.udp = ParsedUdpConfig{};
  ParsedTlsConfig tls;
  tls.insecure = true;
  tls.cert_file = std::string("/some/cert.pem");
  tls.key_file = std::string("/some/key.pem");
  lc.tls = std::move(tls);
  cfg.listeners.value().push_back(std::move(lc));

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
  EXPECT_EQ(resolved.listener.address.getPort(), ParsedSocketConfig::kDefaultPort);
  EXPECT_TRUE(std::holds_alternative<Insecure>(resolved.listener.tlsMode));
  EXPECT_EQ(resolved.listener.endpoint, ParsedListenerConfig::kDefaultEndpoint);
  EXPECT_EQ(resolved.listener.moqtVersions, "");
  EXPECT_THAT(result.value().warnings, IsEmpty());

  // Cache defaults: enabled with default values
  EXPECT_EQ(
      resolved.cache.maxCachedTracks,
      static_cast<size_t>(ParsedCacheConfig::kDefaultMaxTracks)
  );
  EXPECT_EQ(
      resolved.cache.maxCachedGroupsPerTrack,
      static_cast<size_t>(ParsedCacheConfig::kDefaultMaxGroupsPerTrack)
  );
}

TEST(ResolveConfig, FullTls) {
  ParsedConfig cfg;
  ParsedListenerConfig lc;
  lc.name = "production";
  ParsedUdpConfig udp;
  ParsedSocketConfig sock;
  sock.address = std::string("0.0.0.0");
  sock.port = static_cast<uint16_t>(4443);
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
  ParsedCacheConfig cache;
  cache.enabled = false;
  cfg.cache = std::move(cache);

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  const auto& resolved = result.value().config;
  EXPECT_EQ(resolved.cache.maxCachedTracks, 0u);
  EXPECT_EQ(
      resolved.cache.maxCachedGroupsPerTrack,
      static_cast<size_t>(ParsedCacheConfig::kDefaultMaxGroupsPerTrack)
  );
}

TEST(ResolveConfig, CacheCustomValues) {
  auto cfg = makeMinimalInsecureConfig();
  ParsedCacheConfig cache;
  cache.enabled = true;
  cache.max_tracks = uint32_t{200};
  cache.max_groups_per_track = uint32_t{5};
  cfg.cache = std::move(cache);

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
  ParsedUdpConfig udp;
  ParsedSocketConfig sock;
  sock.address = std::string("127.0.0.1");
  sock.port = static_cast<uint16_t>(8080);
  udp.socket = std::move(sock);
  cfg.listeners.value()[0].udp = std::move(udp);

  auto result = resolveConfig(cfg);
  ASSERT_TRUE(result.hasValue());
  const auto& resolved = result.value().config;
  EXPECT_EQ(resolved.listener.address.getPort(), 8080);
  EXPECT_EQ(resolved.listener.address.getAddressStr(), "127.0.0.1");
}

} // namespace
} // namespace openmoq::o_rly::config
