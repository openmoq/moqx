#include <o_rly/config/loader/loader.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <rfl/json.hpp>

#include "test_utils.h"

namespace openmoq::o_rly::config {
namespace {

using test::TempYamlFile;
using ::testing::HasSubstr;

// --- Parse tests ---

TEST(ConfigLoader, MinimalConfig) {
  TempYamlFile yaml(R"(
listeners:
  - name: main
    udp:
      socket:
        address: "::"
        port: 9668
    tls:
      type: insecure
    endpoint: "/moq-relay"
cache:
  enabled: true
  max_tracks: 100
  max_groups_per_track: 3
admin:
  port: 9669
)");

  auto cfg = loadConfig(yaml.path());
  EXPECT_EQ(cfg.listeners.value().size(), 1);
  EXPECT_EQ(cfg.listeners.value()[0].name.value(), "main");

  const auto& sock = cfg.listeners.value()[0].udp.value().socket.value();
  EXPECT_EQ(sock.port.value(), 9668);
  EXPECT_EQ(sock.address.value(), "::");
  EXPECT_EQ(cfg.listeners.value()[0].endpoint.value(), "/moq-relay");

  const auto& cache = cfg.cache.value();
  EXPECT_TRUE(cache.enabled.value());
  EXPECT_EQ(cache.max_tracks.value(), 100);
  EXPECT_EQ(cache.max_groups_per_track.value(), 3);
}

TEST(ConfigLoader, TlsFileConfig) {
  TempYamlFile yaml(R"(
listeners:
  - name: production
    udp:
      socket:
        address: "0.0.0.0"
        port: 4443
    tls:
      type: file
      cert_file: /etc/ssl/cert.pem
      key_file: /etc/ssl/key.pem
    endpoint: "/relay"
    moqt_versions: [14, 16]
cache:
  enabled: true
  max_tracks: 200
  max_groups_per_track: 5
admin:
  port: 9669
)");

  auto cfg = loadConfig(yaml.path());
  const auto& l = cfg.listeners.value()[0];

  EXPECT_EQ(l.name.value(), "production");
  const auto& sock = l.udp.value().socket.value();
  EXPECT_EQ(sock.address.value(), "0.0.0.0");
  EXPECT_EQ(sock.port.value(), 4443);
  EXPECT_EQ(l.endpoint.value(), "/relay");
  ASSERT_TRUE(l.moqt_versions.value().has_value());
  EXPECT_EQ(l.moqt_versions.value()->size(), 2);
  EXPECT_EQ((*l.moqt_versions.value())[0], 14);
  EXPECT_EQ((*l.moqt_versions.value())[1], 16);

  const auto& cache = cfg.cache.value();
  EXPECT_TRUE(cache.enabled.value());
  EXPECT_EQ(cache.max_tracks.value(), 200);
  EXPECT_EQ(cache.max_groups_per_track.value(), 5);
}

TEST(ConfigLoader, TlsDirectoryConfig) {
  TempYamlFile yaml(R"(
listeners:
  - name: multi
    udp:
      socket:
        address: "::"
        port: 4443
    tls:
      type: directory
      cert_dir: /etc/ssl/certs.d/
      default_cert: example.com
    endpoint: "/moq-relay"
cache:
  enabled: true
  max_tracks: 100
  max_groups_per_track: 3
)");

  auto cfg = loadConfig(yaml.path());
  EXPECT_EQ(cfg.listeners.value().size(), 1);
}

TEST(ConfigLoader, TlsDirectoryConfigNoDefault) {
  TempYamlFile yaml(R"(
listeners:
  - name: multi
    udp:
      socket:
        address: "::"
        port: 4443
    tls:
      type: directory
      cert_dir: /etc/ssl/certs.d/
    endpoint: "/moq-relay"
cache:
  enabled: true
  max_tracks: 100
  max_groups_per_track: 3
)");

  auto cfg = loadConfig(yaml.path());
  EXPECT_EQ(cfg.listeners.value().size(), 1);
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
        address: "::"
        port: 8080
    tls:
      type: insecure
    endpoint: "/moq-relay"
cache:
  enabled: false
  max_tracks: 100
  max_groups_per_track: 3
admin:
  port: 9669
)");

  auto cfg = loadConfig(yaml.path());
  EXPECT_EQ(cfg.listeners.value()[0].name.value(), "test");
  EXPECT_FALSE(cfg.cache.value().enabled.value());
}

TEST(ConfigLoader, LoadFromFileNotFound) {
  EXPECT_THROW(loadConfig("/nonexistent/path.yaml"), std::runtime_error);
}

TEST(ConfigLoader, LoadFromFileInvalidYaml) {
  TempYamlFile yaml("not: [valid: yaml: config");
  EXPECT_THROW(loadConfig(yaml.path()), std::runtime_error);
}

// --- Unknown field tests ---

TEST(ConfigLoader, UnknownFieldIgnoredNonStrict) {
  TempYamlFile yaml(R"(
listeners:
  - name: main
    udp:
      socket:
        address: "::"
        port: 9668
    tls:
      type: insecure
    endpoint: "/moq-relay"
    bogus: 42
cache:
  enabled: true
  max_tracks: 100
  max_groups_per_track: 3
admin:
  port: 9669
)");

  EXPECT_NO_THROW(loadConfig(yaml.path()));
}

TEST(ConfigLoader, UnknownFieldRejectedStrict) {
  TempYamlFile yaml(R"(
listeners:
  - name: main
    udp:
      socket:
        address: "::"
        port: 9668
    tls:
      type: insecure
    endpoint: "/moq-relay"
    bogus: 42
cache:
  enabled: true
  max_tracks: 100
  max_groups_per_track: 3
admin:
  port: 9669
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
