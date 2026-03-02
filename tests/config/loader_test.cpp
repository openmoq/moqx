#include <o_rly/config/loader.h>

#include <atomic>
#include <filesystem>
#include <fstream>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <rfl/json.hpp>

namespace openmoq::o_rly::config {
namespace {

using ::testing::Contains;
using ::testing::HasSubstr;

// RAII helper: writes YAML content to a unique temp file, removes it on destruction.
class TempYamlFile {
public:
  explicit TempYamlFile(std::string_view content) {
    static std::atomic<int> counter{0};
    path_ = std::filesystem::temp_directory_path() / ("o_rly_test_" + std::to_string(::getpid()) +
                                                      "_" + std::to_string(counter++) + ".yaml");
    std::ofstream ofs(path_);
    ofs << content;
  }
  ~TempYamlFile() { std::filesystem::remove(path_); }

  TempYamlFile(const TempYamlFile&) = delete;
  TempYamlFile& operator=(const TempYamlFile&) = delete;

  std::string path() const { return path_.string(); }

private:
  std::filesystem::path path_;
};

// --- Parse tests ---

TEST(ConfigLoader, MinimalInsecureConfig) {
  TempYamlFile yaml(R"(
listeners:
  - name: main
    udp: {}
    tls_credentials:
      insecure: true
)");

  auto lr = loadConfig(yaml.path());
  const auto& cfg = lr.config;
  EXPECT_EQ(cfg.listeners.value().size(), 1);
  EXPECT_EQ(cfg.listeners.value()[0].name.value(), "main");
  EXPECT_TRUE(cfg.listeners.value()[0].tls_credentials.value()->insecureOrDefault());

  // Defaults via accessors
  auto sock = cfg.listeners.value()[0].udp.value()->socketOrDefault();
  EXPECT_EQ(sock.portOrDefault(), 9668);
  EXPECT_EQ(sock.addressOrDefault(), "::");
  EXPECT_EQ(cfg.listeners.value()[0].endpointOrDefault(), "/moq-relay");

  // Cache defaults
  auto cache = cfg.cacheOrDefault();
  EXPECT_TRUE(cache.enabledOrDefault());
  EXPECT_EQ(cache.maxTracksOrDefault(), 100);
  EXPECT_EQ(cache.maxGroupsPerTrackOrDefault(), 3);
}

TEST(ConfigLoader, FullConfig) {
  TempYamlFile yaml(R"(
listeners:
  - name: production
    udp:
      socket:
        address: "0.0.0.0"
        port: 4443
    tls_credentials:
      cert_file: /etc/ssl/cert.pem
      key_file: /etc/ssl/key.pem
      insecure: false
    endpoint: "/relay"
    moqt_versions: [14, 16]
cache:
  enabled: true
  max_tracks: 200
  max_groups_per_track: 5
)");

  auto lr = loadConfig(yaml.path());
  const auto& cfg = lr.config;
  const auto& l = cfg.listeners.value()[0];

  EXPECT_EQ(l.name.value(), "production");
  auto sock = l.udp.value()->socketOrDefault();
  EXPECT_EQ(sock.addressOrDefault(), "0.0.0.0");
  EXPECT_EQ(sock.portOrDefault(), 4443);
  EXPECT_EQ(l.tls_credentials.value()->cert_file.value().value(), "/etc/ssl/cert.pem");
  EXPECT_EQ(l.tls_credentials.value()->key_file.value().value(), "/etc/ssl/key.pem");
  EXPECT_FALSE(l.tls_credentials.value()->insecureOrDefault());
  EXPECT_EQ(l.endpointOrDefault(), "/relay");
  ASSERT_TRUE(l.moqt_versions.value().has_value());
  EXPECT_EQ(l.moqt_versions.value()->size(), 2);
  EXPECT_EQ((*l.moqt_versions.value())[0], 14);
  EXPECT_EQ((*l.moqt_versions.value())[1], 16);

  auto cache = cfg.cacheOrDefault();
  EXPECT_TRUE(cache.enabledOrDefault());
  EXPECT_EQ(cache.maxTracksOrDefault(), 200);
  EXPECT_EQ(cache.maxGroupsPerTrackOrDefault(), 5);
}

// --- moqtVersionsToString tests ---

TEST(ConfigLoader, VersionsToStringEmpty) {
  ListenerConfig lc;
  lc.name = "test";
  EXPECT_EQ(moqtVersionsToString(lc), "");
}

TEST(ConfigLoader, VersionsToStringSingle) {
  ListenerConfig lc;
  lc.name = "test";
  lc.moqt_versions = std::vector<uint32_t>{14};
  EXPECT_EQ(moqtVersionsToString(lc), "14");
}

TEST(ConfigLoader, VersionsToStringMultiple) {
  ListenerConfig lc;
  lc.name = "test";
  lc.moqt_versions = std::vector<uint32_t>{14, 16};
  EXPECT_EQ(moqtVersionsToString(lc), "14,16");
}

// --- Validation error tests ---

TEST(ConfigValidation, NoListeners) {
  Config cfg;
  auto errors = validateConfig(cfg);
  ASSERT_FALSE(errors.empty());
  EXPECT_THAT(errors, Contains(HasSubstr("At least one listener")));
}

TEST(ConfigValidation, MissingUdp) {
  Config cfg;
  ListenerConfig lc;
  lc.name = "test";
  lc.tls_credentials = TlsCredentials{};
  lc.tls_credentials.value()->insecure = true;
  cfg.listeners.value().push_back(std::move(lc));

  auto errors = validateConfig(cfg);
  EXPECT_THAT(errors, Contains(HasSubstr("udp")));
}

TEST(ConfigValidation, InsecureFalseNoCerts) {
  Config cfg;
  ListenerConfig lc;
  lc.name = "test";
  lc.udp = UdpConfig{};
  lc.tls_credentials = TlsCredentials{};
  // insecure defaults to false, no cert/key set
  cfg.listeners.value().push_back(std::move(lc));

  auto errors = validateConfig(cfg);
  EXPECT_THAT(errors, Contains(HasSubstr("cert_file")));
}

TEST(ConfigValidation, PortZero) {
  Config cfg;
  ListenerConfig lc;
  lc.name = "test";
  UdpConfig udp;
  SocketConfig sock;
  sock.port = static_cast<uint16_t>(0);
  udp.socket = std::move(sock);
  lc.udp = std::move(udp);
  lc.tls_credentials = TlsCredentials{};
  lc.tls_credentials.value()->insecure = true;
  cfg.listeners.value().push_back(std::move(lc));

  auto errors = validateConfig(cfg);
  EXPECT_THAT(errors, Contains(HasSubstr("port")));
}

TEST(ConfigValidation, NoTlsCredentials) {
  Config cfg;
  ListenerConfig lc;
  lc.name = "test";
  lc.udp = UdpConfig{};
  // No tls_credentials set at all
  cfg.listeners.value().push_back(std::move(lc));

  auto errors = validateConfig(cfg);
  EXPECT_THAT(errors, Contains(HasSubstr("tls_credentials")));
}

// --- Warning tests ---

TEST(ConfigWarnings, InsecureWithCerts) {
  Config cfg;
  ListenerConfig lc;
  lc.name = "test";
  lc.udp = UdpConfig{};
  TlsCredentials tls;
  tls.insecure = true;
  tls.cert_file = std::string("/some/cert.pem");
  tls.key_file = std::string("/some/key.pem");
  lc.tls_credentials = std::move(tls);
  cfg.listeners.value().push_back(std::move(lc));

  auto warnings = warnConfig(cfg);
  ASSERT_FALSE(warnings.empty());
  EXPECT_THAT(warnings, Contains(HasSubstr("ignored")));
}

// --- Schema generation test ---

TEST(ConfigSchema, GeneratesValidJson) {
  auto schema = generateSchema();
  EXPECT_FALSE(schema.empty());

  // Verify it's valid JSON by attempting to parse it
  auto parsed = rfl::json::read<rfl::Generic>(schema);
  EXPECT_TRUE(parsed.has_value()) << "Schema is not valid JSON";

  // Check it contains expected fields
  EXPECT_THAT(schema, HasSubstr("listeners"));
  EXPECT_THAT(schema, HasSubstr("cache"));
  EXPECT_THAT(schema, HasSubstr("Bind address"));
}

// --- Load from file test ---

TEST(ConfigLoader, LoadFromFile) {
  TempYamlFile yaml(R"(
listeners:
  - name: test
    udp:
      socket:
        port: 8080
    tls_credentials:
      insecure: true
cache:
  enabled: false
)");

  auto lr = loadConfig(yaml.path());
  EXPECT_EQ(lr.config.listeners.value()[0].name.value(), "test");
  EXPECT_FALSE(lr.config.cacheOrDefault().enabledOrDefault());
}

TEST(ConfigLoader, LoadFromFileNotFound) {
  EXPECT_THROW(loadConfig("/nonexistent/path.yaml"), std::runtime_error);
}

TEST(ConfigLoader, LoadFromFileInvalidYaml) {
  TempYamlFile yaml("not: [valid: yaml: config");
  EXPECT_THROW(loadConfig(yaml.path()), std::runtime_error);
}

// --- Unknown field detection tests ---

TEST(ConfigLoader, UnknownFieldWarning) {
  TempYamlFile yaml(R"(
listeners:
  - name: main
    udp: {}
    tls_credentials:
      insecure: true
    bogus: 42
)");

  auto lr = loadConfig(yaml.path());
  EXPECT_THAT(lr.warnings, Contains(HasSubstr("bogus")));
}

TEST(ConfigLoader, UnknownFieldStrict) {
  TempYamlFile yaml(R"(
listeners:
  - name: main
    udp: {}
    tls_credentials:
      insecure: true
    bogus: 42
)");

  EXPECT_THROW(loadConfig(yaml.path(), /*strict=*/true), std::runtime_error);
}

#ifdef CONFIG_EXAMPLE_PATH
TEST(ConfigLoader, ExampleYamlValid) {
  EXPECT_NO_THROW(loadConfig(CONFIG_EXAMPLE_PATH, /*strict=*/true));
}
#endif

} // namespace
} // namespace openmoq::o_rly::config
