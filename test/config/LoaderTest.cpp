/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "config/loader/Loader.h"
#include "config/loader/ConfigResolver.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <rfl/json.hpp>

#include "TestUtils.h"

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
      insecure: true
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
)");

  auto cfg = loadConfig(yaml.path());
  EXPECT_EQ(cfg.listeners.value().size(), 1);
  EXPECT_EQ(cfg.listeners.value()[0].name.value(), "main");
  EXPECT_TRUE(cfg.listeners.value()[0].tls.value().insecure.value());

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

TEST(ConfigLoader, ServiceListeners) {
  TempYamlFile yaml(R"(
listeners:
  - name: public
    udp:
      socket:
        address: "::"
        port: 9668
    tls:
      insecure: true
    endpoint: "/moq-relay"
services:
  bound:
    listeners: [public]
    match:
      - authority: {any: true}
        path: {prefix: "/"}
  unbound:
    match:
      - authority: {any: true}
        path: {prefix: "/x/"}
)");

  auto cfg = loadConfig(yaml.path());
  const auto& bound = cfg.services.value().at("bound").listeners.value();
  ASSERT_TRUE(bound.has_value());
  EXPECT_THAT(*bound, ::testing::ElementsAre("public"));
  EXPECT_FALSE(cfg.services.value().at("unbound").listeners.value().has_value());
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
    moqt_versions: [16, 18]
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
)");

  auto cfg = loadConfig(yaml.path());
  const auto& l = cfg.listeners.value()[0];

  EXPECT_EQ(l.name.value(), "production");
  const auto& sock = l.udp.value().socket.value();
  EXPECT_EQ(sock.address.value(), "0.0.0.0");
  EXPECT_EQ(sock.port.value(), 4443);
  EXPECT_EQ(l.tls.value().cert_file.value().value(), "/etc/ssl/cert.pem");
  EXPECT_EQ(l.tls.value().key_file.value().value(), "/etc/ssl/key.pem");
  EXPECT_FALSE(l.tls.value().insecure.value());
  EXPECT_EQ(l.endpoint.value(), "/relay");
  ASSERT_TRUE(l.moqt_versions.value().has_value());
  EXPECT_EQ(l.moqt_versions.value()->size(), 2);
  EXPECT_EQ((*l.moqt_versions.value())[0], 16);
  EXPECT_EQ((*l.moqt_versions.value())[1], 18);

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
      insecure: true
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
      insecure: true
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
      insecure: true
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

// The YAML reader wraps a negative uint64_t; the resolver's upper bound rejects it.
TEST(ConfigLoader, NegativeH3TxnTimeoutRejected) {
  TempYamlFile yaml(R"(
listeners:
  - name: main
    udp:
      socket:
        address: "::"
        port: 9668
    tls:
      insecure: true
    endpoint: "/moq-relay"
    mvfst:
      h3_txn_timeout_ms: -1
services:
  default:
    match:
      - authority: {any: true}
        path: {prefix: "/"}
admin:
  port: 9669
  address: "::1"
  plaintext: true
)");

  auto result = resolveConfig(loadConfig(yaml.path()));
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("h3_txn_timeout_ms"));
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
      insecure: true
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

// --- Listener map form and duplicate keys ---

constexpr std::string_view kListenerBody = R"(
    udp: {socket: {address: "::", port: 9668}}
    tls: {insecure: true}
    endpoint: "/moq-relay")";

constexpr std::string_view kServices = R"(
services:
  default:
    match: [{authority: {any: true}, path: {prefix: "/"}}]
)";

std::string listenersYaml(std::string_view entries) {
  return "listeners:" + std::string(entries) + std::string(kServices);
}

TEST(ConfigLoader, ListenersMapFormNamesFromKeys) {
  TempYamlFile yaml(listenersYaml(
      "\n  public:" + std::string(kListenerBody) + "\n  internal:" + std::string(kListenerBody)
  ));

  auto cfg = loadConfig(yaml.path(), /*strict=*/true);
  ASSERT_EQ(cfg.listeners.value().size(), 2);
  EXPECT_EQ(cfg.listeners.value()[0].name.value(), "public");
  EXPECT_EQ(cfg.listeners.value()[1].name.value(), "internal");
  EXPECT_EQ(cfg.listeners.value()[1].endpoint.value(), "/moq-relay");
  EXPECT_FALSE(cfg.listeners.value().listForm());
}

TEST(ConfigLoader, ListenersListFormRecorded) {
  TempYamlFile yaml(listenersYaml("\n  - name: main" + std::string(kListenerBody)));

  auto cfg = loadConfig(yaml.path(), /*strict=*/true);
  ASSERT_EQ(cfg.listeners.value().size(), 1);
  EXPECT_EQ(cfg.listeners.value()[0].name.value(), "main");
  EXPECT_TRUE(cfg.listeners.value().listForm());
}

TEST(ConfigLoader, ListenersMapEntryWithNameRejectedWhenStrict) {
  TempYamlFile yaml(listenersYaml("\n  public:\n    name: public" + std::string(kListenerBody)));
  try {
    loadConfig(yaml.path(), /*strict=*/true);
    FAIL() << "expected a parse error";
  } catch (const std::runtime_error& e) {
    EXPECT_THAT(e.what(), HasSubstr("Value named 'name' not used"));
  }
}

TEST(ConfigLoader, ListenersListEntryWithoutNameRejected) {
  TempYamlFile yaml(listenersYaml(R"(
  - udp: {socket: {address: "::", port: 9668}}
    tls: {insecure: true}
    endpoint: "/moq-relay")"));
  try {
    loadConfig(yaml.path());
    FAIL() << "expected a parse error";
  } catch (const std::runtime_error& e) {
    EXPECT_THAT(e.what(), HasSubstr("Field named 'name' not found"));
  }
}

TEST(ConfigLoader, ListenersListEntryWithEmptyNameRejected) {
  TempYamlFile yaml(listenersYaml("\n  - name: \"\"" + std::string(kListenerBody)));
  try {
    loadConfig(yaml.path());
    FAIL() << "expected a parse error";
  } catch (const std::runtime_error& e) {
    EXPECT_THAT(e.what(), HasSubstr("listeners[0]: name must not be empty"));
  }
}

TEST(ConfigLoader, ListenersMapEmptyKeyRejected) {
  TempYamlFile yaml(listenersYaml("\n  \"\":" + std::string(kListenerBody)));
  try {
    loadConfig(yaml.path());
    FAIL() << "expected a parse error";
  } catch (const std::runtime_error& e) {
    EXPECT_THAT(e.what(), HasSubstr("listeners: a listener name must not be empty"));
  }
}

TEST(ConfigLoader, DuplicateListenerKeyRejected) {
  TempYamlFile yaml(listenersYaml(
      "\n  public:" + std::string(kListenerBody) + "\n  public:" + std::string(kListenerBody)
  ));
  try {
    loadConfig(yaml.path());
    FAIL() << "expected a parse error";
  } catch (const std::runtime_error& e) {
    EXPECT_THAT(e.what(), HasSubstr("duplicate key 'public'"));
  }
}

TEST(ConfigLoader, DuplicateServiceKeyRejected) {
  TempYamlFile yaml(listenersYaml("\n  main:" + std::string(kListenerBody)) + R"(
  default:
    match: [{authority: {any: true}, path: {prefix: "/x/"}}]
)");
  try {
    loadConfig(yaml.path());
    FAIL() << "expected a parse error";
  } catch (const std::runtime_error& e) {
    EXPECT_THAT(e.what(), HasSubstr("duplicate key 'default'"));
  }
}

TEST(ConfigLoader, DuplicateNestedFieldRejected) {
  TempYamlFile yaml(listenersYaml(R"(
  main:
    udp: {socket: {address: "::", port: 9668, port: 9669}}
    tls: {insecure: true}
    endpoint: "/moq-relay")"));
  try {
    loadConfig(yaml.path());
    FAIL() << "expected a parse error";
  } catch (const std::runtime_error& e) {
    EXPECT_THAT(e.what(), HasSubstr("duplicate key 'port'"));
  }
}

TEST(ConfigLoader, NullListenerKeysCollide) {
  TempYamlFile yaml(listenersYaml(
      "\n  ~:" + std::string(kListenerBody) + "\n  null:" + std::string(kListenerBody)
  ));
  try {
    loadConfig(yaml.path());
    FAIL() << "expected a parse error";
  } catch (const std::runtime_error& e) {
    EXPECT_THAT(e.what(), HasSubstr("duplicate key 'null'"));
  }
}

TEST(ConfigLoader, DuplicateAliasKeyRejected) {
  TempYamlFile yaml(listenersYaml(
      "\n  &name public:" + std::string(kListenerBody) + "\n  *name :" + std::string(kListenerBody)
  ));
  try {
    loadConfig(yaml.path());
    FAIL() << "expected a parse error";
  } catch (const std::runtime_error& e) {
    EXPECT_THAT(e.what(), HasSubstr("duplicate key 'public'"));
  }
}

TEST(ConfigLoader, SelfReferencingAnchorAccepted) {
  TempYamlFile yaml(
      listenersYaml("\n  main:" + std::string(kListenerBody)) + "x-extension: &loop {self: *loop}\n"
  );
  EXPECT_NO_THROW(loadConfig(yaml.path()));
}

TEST(ConfigLoader, NestedAliasFanOutAccepted) {
  // Expanding the aliases would visit 10^9 nodes.
  std::string ext = "x-extension:\n  l0: &l0 [0, 0, 0, 0, 0, 0, 0, 0, 0, 0]\n";
  for (int i = 1; i <= 8; ++i) {
    std::string prev = "*l" + std::to_string(i - 1);
    ext += "  l" + std::to_string(i) + ": &l" + std::to_string(i) + " [" + prev;
    for (int j = 1; j < 10; ++j) {
      ext += ", " + prev;
    }
    ext += "]\n";
  }
  TempYamlFile yaml(listenersYaml("\n  main:" + std::string(kListenerBody)) + ext);
  EXPECT_NO_THROW(loadConfig(yaml.path()));
}

TEST(ConfigLoader, CollectionKeyRejected) {
  TempYamlFile yaml(
      listenersYaml("\n  main:" + std::string(kListenerBody)) + "x-extension: {? [a, b]: 1}\n"
  );
  try {
    loadConfig(yaml.path());
    FAIL() << "expected a parse error";
  } catch (const std::runtime_error& e) {
    EXPECT_THAT(e.what(), HasSubstr("must be a scalar"));
  }
}

TEST(ConfigLoader, AliasToCollectionKeyRejected) {
  TempYamlFile yaml(
      listenersYaml("\n  main:" + std::string(kListenerBody)) +
      "x-extension:\n  base: &k [a]\n  *k : 1\n"
  );
  try {
    loadConfig(yaml.path());
    FAIL() << "expected a parse error";
  } catch (const std::runtime_error& e) {
    EXPECT_THAT(e.what(), HasSubstr("must be a scalar"));
  }
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
      insecure: true
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
      insecure: true
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
)");

  EXPECT_THROW(loadConfig(yaml.path(), /*strict=*/true), std::runtime_error);
}

TEST(ConfigLoader, UnknownQuicStackRejected) {
  TempYamlFile yaml(R"(
listeners:
  - name: main
    udp:
      socket:
        address: "::"
        port: 9668
    tls:
      insecure: true
    endpoint: "/moq-relay"
    quic_stack: qmux
services:
  default:
    match:
      - authority: {any: true}
        path: {prefix: "/"}
)");

  try {
    loadConfig(yaml.path());
    FAIL() << "expected an unknown quic_stack to fail the load";
  } catch (const std::runtime_error& e) {
    EXPECT_THAT(e.what(), HasSubstr("qmux"));
    EXPECT_THAT(e.what(), HasSubstr("proxygen_qmux"));
  }
}

#ifdef CONFIG_EXAMPLE_PATH
TEST(ConfigLoader, ExampleYamlValid) {
  EXPECT_NO_THROW(loadConfig(CONFIG_EXAMPLE_PATH, /*strict=*/true));
}
#endif

} // namespace
} // namespace openmoq::moqx::config
