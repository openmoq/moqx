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

TEST(ConfigLoader, MinimalInsecureConfig) {
  TempYamlFile yaml(R"(
listeners:
  - name: main
    udp: {}
    tls:
      insecure: true
)");

  auto cfg = loadConfig(yaml.path());
  EXPECT_EQ(cfg.listeners.value().size(), 1);
  EXPECT_EQ(cfg.listeners.value()[0].name.value(), "main");
  EXPECT_TRUE(cfg.listeners.value()[0].tls.value()->insecureOrDefault());

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
    tls:
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

  auto cfg = loadConfig(yaml.path());
  const auto& l = cfg.listeners.value()[0];

  EXPECT_EQ(l.name.value(), "production");
  auto sock = l.udp.value()->socketOrDefault();
  EXPECT_EQ(sock.addressOrDefault(), "0.0.0.0");
  EXPECT_EQ(sock.portOrDefault(), 4443);
  EXPECT_EQ(l.tls.value()->cert_file.value().value(), "/etc/ssl/cert.pem");
  EXPECT_EQ(l.tls.value()->key_file.value().value(), "/etc/ssl/key.pem");
  EXPECT_FALSE(l.tls.value()->insecureOrDefault());
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
    tls:
      insecure: true
cache:
  enabled: false
)");

  auto cfg = loadConfig(yaml.path());
  EXPECT_EQ(cfg.listeners.value()[0].name.value(), "test");
  EXPECT_FALSE(cfg.cacheOrDefault().enabledOrDefault());
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
    udp: {}
    tls:
      insecure: true
    bogus: 42
)");

  EXPECT_NO_THROW(loadConfig(yaml.path()));
}

TEST(ConfigLoader, UnknownFieldRejectedStrict) {
  TempYamlFile yaml(R"(
listeners:
  - name: main
    udp: {}
    tls:
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
