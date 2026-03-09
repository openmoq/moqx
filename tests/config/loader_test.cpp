#include <moqx/config/loader/loader.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <rfl/json.hpp>

#include "test_utils.h"

namespace openmoq::moqx::config {
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
services:
  default:
    match:
      - authority: {any: true}
        path: {prefix: "/"}
    cache:
      enabled: true
      max_tracks: 100
      max_groups_per_track: 3
admin:
  port: 9669
  address: "::1"
  plaintext: true
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

  ASSERT_EQ(cfg.services.value().size(), 1);
  const auto& svc = cfg.services.value().at("default");
  ASSERT_TRUE(svc.cache.value().has_value());
  EXPECT_EQ(svc.cache.value()->enabled.value(), true);
  EXPECT_EQ(svc.cache.value()->max_tracks.value(), 100);
  EXPECT_EQ(svc.cache.value()->max_groups_per_track.value(), 3);
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
services:
  live:
    match:
      - authority: {exact: "live.example.com"}
        path: {exact: "/moq-relay"}
    cache:
      enabled: true
      max_tracks: 200
      max_groups_per_track: 5
admin:
  port: 9669
  address: "::1"
  plaintext: true
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

  ASSERT_EQ(cfg.services.value().size(), 1);
  const auto& svc = cfg.services.value().at("live");
  ASSERT_TRUE(svc.cache.value().has_value());
  EXPECT_EQ(svc.cache.value()->enabled.value(), true);
  EXPECT_EQ(svc.cache.value()->max_tracks.value(), 200);
  EXPECT_EQ(svc.cache.value()->max_groups_per_track.value(), 5);
}

TEST(ConfigLoader, ServicesWithAuthorityAndPath) {
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
services:
  live:
    match:
      - authority: {exact: "live.example.com"}
        path: {exact: "/moq-relay"}
      - authority: {wildcard: "*.live.example.com"}
        path: {prefix: "/live/"}
    cache:
      enabled: true
      max_tracks: 500
      max_groups_per_track: 10
  catch-all:
    match:
      - authority: {any: true}
        path: {prefix: "/"}
    cache:
      enabled: true
      max_tracks: 100
      max_groups_per_track: 3
)");

  auto cfg = loadConfig(yaml.path());
  ASSERT_EQ(cfg.services.value().size(), 2);

  const auto& svc0 = cfg.services.value().at("live");
  ASSERT_EQ(svc0.match.value().size(), 2);

  // First match entry: exact authority + exact path
  const auto& m0 = svc0.match.value()[0];
  std::string m0AuthExact;
  m0.authority.value().visit([&](const auto& alt) {
    using A = std::decay_t<decltype(alt)>;
    if constexpr (std::is_same_v<A, ParsedServiceConfig::MatchRule::ExactAuthority>) {
      m0AuthExact = alt.exact.value();
    }
  });
  EXPECT_EQ(m0AuthExact, "live.example.com");

  std::string m0PathExact;
  m0.path.value().visit([&](const auto& alt) {
    using P = std::decay_t<decltype(alt)>;
    if constexpr (std::is_same_v<P, ParsedServiceConfig::MatchRule::ExactPath>) {
      m0PathExact = alt.exact.value();
    }
  });
  EXPECT_EQ(m0PathExact, "/moq-relay");

  // Second match entry: wildcard authority + prefix path
  const auto& m1 = svc0.match.value()[1];
  std::string m1AuthWild;
  m1.authority.value().visit([&](const auto& alt) {
    using A = std::decay_t<decltype(alt)>;
    if constexpr (std::is_same_v<A, ParsedServiceConfig::MatchRule::WildcardAuthority>) {
      m1AuthWild = alt.wildcard.value();
    }
  });
  EXPECT_EQ(m1AuthWild, "*.live.example.com");

  std::string m1PathPrefix;
  m1.path.value().visit([&](const auto& alt) {
    using P = std::decay_t<decltype(alt)>;
    if constexpr (std::is_same_v<P, ParsedServiceConfig::MatchRule::PrefixPath>) {
      m1PathPrefix = alt.prefix.value();
    }
  });
  EXPECT_EQ(m1PathPrefix, "/live/");

  const auto& svc1 = cfg.services.value().at("catch-all");
  ASSERT_EQ(svc1.match.value().size(), 1);
  bool isAnyAuth = false;
  svc1.match.value()[0].authority.value().visit([&](const auto& alt) {
    using A = std::decay_t<decltype(alt)>;
    if constexpr (std::is_same_v<A, ParsedServiceConfig::MatchRule::AnyAuthority>) {
      isAnyAuth = true;
    }
  });
  EXPECT_TRUE(isAnyAuth);
}

TEST(ConfigLoader, ServicesWithAnyAuthority) {
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
services:
  default:
    match:
      - authority: {any: true}
        path: {prefix: "/"}
    cache:
      enabled: true
      max_tracks: 100
      max_groups_per_track: 3
)");

  auto cfg = loadConfig(yaml.path());
  ASSERT_EQ(cfg.services.value().size(), 1);
  const auto& svc = cfg.services.value().at("default");
  ASSERT_EQ(svc.match.value().size(), 1);
  bool isAnyAuth = false;
  bool anyVal = false;
  svc.match.value()[0].authority.value().visit([&](const auto& alt) {
    using A = std::decay_t<decltype(alt)>;
    if constexpr (std::is_same_v<A, ParsedServiceConfig::MatchRule::AnyAuthority>) {
      isAnyAuth = true;
      anyVal = alt.any.value();
    }
  });
  EXPECT_TRUE(isAnyAuth);
  EXPECT_TRUE(anyVal);

  std::string pathPrefix;
  svc.match.value()[0].path.value().visit([&](const auto& alt) {
    using P = std::decay_t<decltype(alt)>;
    if constexpr (std::is_same_v<P, ParsedServiceConfig::MatchRule::PrefixPath>) {
      pathPrefix = alt.prefix.value();
    }
  });
  EXPECT_EQ(pathPrefix, "/");
}

TEST(ConfigLoader, ServiceDefaults) {
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
service_defaults:
  cache:
    enabled: true
    max_tracks: 50
    max_groups_per_track: 2
services:
  default:
    match:
      - authority: {any: true}
        path: {prefix: "/"}
)");

  auto cfg = loadConfig(yaml.path());
  ASSERT_TRUE(cfg.service_defaults.value().has_value());
  ASSERT_TRUE(cfg.service_defaults.value()->cache.value().has_value());
  EXPECT_EQ(cfg.service_defaults.value()->cache.value()->max_tracks.value(), 50);
  EXPECT_EQ(cfg.service_defaults.value()->cache.value()->max_groups_per_track.value(), 2);

  ASSERT_EQ(cfg.services.value().size(), 1);
  EXPECT_FALSE(cfg.services.value().at("default").cache.value().has_value());
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
services:
  default:
    match:
      - authority: {any: true}
        path: {prefix: "/"}
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
services:
  default:
    match:
      - authority: {any: true}
        path: {prefix: "/"}
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
  EXPECT_THAT(schema, HasSubstr("services"));
  EXPECT_THAT(schema, HasSubstr("Bind address"));
  EXPECT_THAT(schema, HasSubstr("Match rules"));
  EXPECT_THAT(schema, HasSubstr("Path matcher"));
  // AuthorityMatch should produce anyOf with exact/wildcard/any
  EXPECT_THAT(schema, HasSubstr("anyOf"));
  EXPECT_THAT(schema, HasSubstr("\"any\""));
  EXPECT_THAT(schema, HasSubstr("\"exact\""));
  EXPECT_THAT(schema, HasSubstr("\"wildcard\""));
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
services:
  default:
    match:
      - authority: {any: true}
        path: {prefix: "/"}
    cache:
      enabled: false
      max_tracks: 100
      max_groups_per_track: 3
admin:
  port: 9669
  address: "::1"
  plaintext: true
admin:
  port: 9669
)");

  auto cfg = loadConfig(yaml.path());
  EXPECT_EQ(cfg.listeners.value()[0].name.value(), "test");
  ASSERT_TRUE(cfg.services.value().at("default").cache.value().has_value());
  EXPECT_EQ(cfg.services.value().at("default").cache.value()->enabled.value(), false);
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
services:
  default:
    match:
      - authority: {any: true}
        path: {prefix: "/"}
    cache:
      enabled: true
      max_tracks: 100
      max_groups_per_track: 3
admin:
  port: 9669
  address: "::1"
  plaintext: true
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
services:
  default:
    match:
      - authority: {any: true}
        path: {prefix: "/"}
    cache:
      enabled: true
      max_tracks: 100
      max_groups_per_track: 3
admin:
  port: 9669
  address: "::1"
  plaintext: true
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
} // namespace openmoq::moqx::config
