/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "config/ConfigSerializer.h"
#include "config/loader/ConfigResolver.h"
#include "plugin/TestPlugin.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <rfl/yaml.hpp>

namespace openmoq::moqx::plugin::test {
namespace {

using ::testing::HasSubstr;

constexpr std::string_view kBase = R"(
listeners:
  - name: main
    udp:
      socket:
        address: "::"
        port: 9668
    tls:
      insecure: true
    endpoint: "/moq-relay"
)";

constexpr std::string_view kServices = R"(
services:
  live:
    match:
      - authority: {exact: "live.example.com"}
        path: {prefix: "/"}
    cache: {enabled: false, max_tracks: 1, max_groups_per_track: 1}
    max_subscription_buffer_kb: 1024
)";

class PluginConfigTest : public ::testing::Test {
protected:
  void SetUp() override {
    currentScript() = std::make_shared<Script>();
    registry_ = {testType("test"), serviceOnly_};
  }

  // `top` goes before services:, `svcPlugins` under services.live.
  folly::Expected<config::ResolvedConfig, std::string>
  resolve(std::string_view top, std::string_view svcExtra = "", bool strict = false) {
    std::string yaml =
        std::string(kBase) + std::string(top) + std::string(kServices) + std::string(svcExtra);
    auto parsed = strict ? rfl::yaml::read<config::ParsedConfig, rfl::NoExtraFields>(yaml)
                         : rfl::yaml::read<config::ParsedConfig>(yaml);
    if (!parsed) {
      return folly::makeUnexpected("parse: " + parsed.error().what());
    }
    return config::resolveConfig(*parsed, {.registry = &registry_, .strict = strict});
  }

  std::shared_ptr<const PluginType> serviceOnly_ = [] {
    auto type = std::make_shared<PluginType>(*testType("service-only"));
    type->processScope = false;
    return std::shared_ptr<const PluginType>(type);
  }();
  Registry registry_;
};

TEST_F(PluginConfigTest, ResolvesEntriesWithDefaults) {
  auto res = resolve(
      "plugins:\n  - type: test\n",
      "    plugins:\n"
      "      - type: test\n        id: second\n        rank: -3\n        config: {tag: 7}\n"
  );
  ASSERT_TRUE(res.hasValue()) << res.error();
  const auto& cfg = res->config;
  ASSERT_EQ(cfg.plugins.size(), 1);
  EXPECT_EQ(cfg.plugins[0].id, "test");
  EXPECT_EQ(cfg.plugins[0].rank, 0);
  const auto& svc = cfg.services.at("live");
  ASSERT_EQ(svc.plugins.size(), 1);
  EXPECT_EQ(svc.plugins[0].id, "second");
  EXPECT_EQ(svc.plugins[0].rank, -3);
  EXPECT_EQ(static_cast<const TestConfig*>(svc.plugins[0].config.get())->tag, 7);
  EXPECT_EQ(svc.admissionTimeout, std::chrono::milliseconds(5000));
  EXPECT_EQ(svc.authorizeTimeout, std::chrono::milliseconds(5000));
}

TEST_F(PluginConfigTest, UnknownType) {
  auto res = resolve("plugins:\n  - type: nope\n");
  ASSERT_TRUE(res.hasError());
  EXPECT_THAT(res.error(), HasSubstr("plugins[0] (nope): unknown plugin type"));
}

TEST_F(PluginConfigTest, ScopeNotAllowed) {
  auto res = resolve("plugins:\n  - type: service-only\n");
  ASSERT_TRUE(res.hasError());
  EXPECT_THAT(res.error(), HasSubstr("plugins[0] (service-only): not allowed at process scope"));
}

TEST_F(PluginConfigTest, TypedConfigErrorNamesEntry) {
  auto res = resolve("", "    plugins:\n      - type: test\n        config: {tag: \"x\"}\n");
  ASSERT_TRUE(res.hasError());
  EXPECT_THAT(res.error(), HasSubstr("Service 'live': plugins[0] (test): config:"));
  EXPECT_THAT(res.error(), HasSubstr("tag"));
}

TEST_F(PluginConfigTest, FactoryErrorIsAConfigError) {
  auto res = resolve("", "    plugins:\n      - type: test\n        config: {tag: -1}\n");
  ASSERT_TRUE(res.hasError());
  EXPECT_THAT(res.error(), HasSubstr("plugins[0] (test): tag must be >= 0"));
}

TEST_F(PluginConfigTest, UnknownConfigFieldOnlyFailsWhenStrict) {
  constexpr std::string_view svc = "    plugins:\n      - type: test\n        config: {bogus: 1}\n";
  EXPECT_TRUE(resolve("", svc).hasValue());
  auto strict = resolve("", svc, /*strict=*/true);
  ASSERT_TRUE(strict.hasError());
  EXPECT_THAT(strict.error(), HasSubstr("bogus"));
}

TEST_F(PluginConfigTest, DuplicateIds) {
  auto top = resolve("plugins:\n  - type: test\n  - type: test\n");
  ASSERT_TRUE(top.hasError());
  EXPECT_THAT(top.error(), HasSubstr("Duplicate top-level plugin id 'test'"));

  auto svc = resolve(
      "",
      "    plugins:\n      - type: test\n        id: a\n      - type: test\n        id: a\n"
  );
  ASSERT_TRUE(svc.hasError());
  EXPECT_THAT(svc.error(), HasSubstr("Service 'live': duplicate plugin id 'a'"));
}

TEST_F(PluginConfigTest, ServiceIdMustDifferFromProcessId) {
  auto res = resolve("plugins:\n  - type: test\n", "    plugins:\n      - type: test\n");
  ASSERT_TRUE(res.hasError());
  EXPECT_THAT(res.error(), HasSubstr("plugin id 'test' is also a top-level plugin id"));
}

TEST_F(PluginConfigTest, ServiceScopeCannotDeclareListenerParams) {
  auto res =
      resolve("", "    plugins:\n      - type: test\n        config: {listener_params: true}\n");
  ASSERT_TRUE(res.hasError());
  EXPECT_THAT(res.error(), HasSubstr("listener SETUP parameters, allowed at process scope only"));
  EXPECT_TRUE(resolve("plugins:\n  - type: test\n    config: {listener_params: true}\n").hasValue()
  );
}

TEST_F(PluginConfigTest, ServiceDefaultsMergeIntoServiceEntries) {
  auto merged = resolve(
      "service_defaults:\n"
      "  admission_timeout_ms: 250\n"
      "  plugins:\n"
      "    test: {tag: 3, secret: from-defaults}\n"
      "plugins:\n  - type: test\n    id: top\n",
      "    authorize_timeout_ms: 75\n"
      "    plugins:\n      - type: test\n        config: {secret: own}\n"
  );
  ASSERT_TRUE(merged.hasValue()) << merged.error();
  const auto& svc = merged->config.services.at("live");
  const auto* svcConfig = static_cast<const TestConfig*>(svc.plugins[0].config.get());
  EXPECT_EQ(svcConfig->tag, 3);
  EXPECT_EQ(svcConfig->secret, "own");
  // Defaults never reach process entries.
  const auto* topConfig = static_cast<const TestConfig*>(merged->config.plugins[0].config.get());
  EXPECT_FALSE(topConfig->tag.has_value());
  EXPECT_EQ(svc.admissionTimeout, std::chrono::milliseconds(250));
  EXPECT_EQ(svc.authorizeTimeout, std::chrono::milliseconds(75));
}

// A scalar reaches the plugin as written, whatever YAML would type it as.
TEST_F(PluginConfigTest, ScalarsKeepTheirText) {
  auto res =
      resolve("", "    plugins:\n      - type: test\n        config: {name: 007, secret: 1}\n");
  ASSERT_TRUE(res.hasValue()) << res.error();
  const auto* cfg =
      static_cast<const TestConfig*>(res->config.services.at("live").plugins[0].config.get());
  EXPECT_EQ(cfg->name, "007");
  EXPECT_EQ(cfg->secret, "1");
}

TEST_F(PluginConfigTest, ServiceDefaultsDoNotLeakBetweenServices) {
  auto res = resolve(
      "service_defaults:\n  plugins:\n    test: {tag: 3, secret: shared}\n",
      "    plugins:\n      - type: test\n        config: {tag: 5}\n"
      "  other:\n"
      "    match:\n      - authority: {exact: \"other.example.com\"}\n        path: {prefix: "
      "\"/\"}\n"
      "    cache: {enabled: false, max_tracks: 1, max_groups_per_track: 1}\n"
      "    max_subscription_buffer_kb: 1024\n"
      "    plugins:\n      - type: test\n"
  );
  ASSERT_TRUE(res.hasValue()) << res.error();
  auto config = [&](const std::string& service) {
    return static_cast<const TestConfig*>(res->config.services.at(service).plugins[0].config.get());
  };
  EXPECT_EQ(config("live")->tag, 5);
  EXPECT_EQ(config("live")->secret, "shared");
  EXPECT_EQ(config("other")->tag, 3);
  EXPECT_EQ(config("other")->secret, "shared");
}

TEST_F(PluginConfigTest, ServiceDefaultsRejectUnknownTypeAndField) {
  auto res = resolve("service_defaults:\n  plugins:\n    nope: {}\n    test: {bogus: 1}\n");
  ASSERT_TRUE(res.hasError());
  EXPECT_THAT(res.error(), HasSubstr("service_defaults.plugins.nope: unknown plugin type"));
  EXPECT_THAT(res.error(), HasSubstr("service_defaults.plugins.test: unknown field 'bogus'"));
}

TEST_F(PluginConfigTest, ZeroTimeoutRejected) {
  auto res = resolve("", "    admission_timeout_ms: 0\n");
  ASSERT_TRUE(res.hasError());
  EXPECT_THAT(res.error(), HasSubstr("Service 'live': admission_timeout_ms must be >= 1"));
}

class RecordingSink : public config::ConfigSink {
public:
  void beginObject(std::string_view key) override { out += "{" + std::string(key) + ":"; }
  void endObject() override { out += "}"; }
  void beginArray(std::string_view key) override { out += "[" + std::string(key) + ":"; }
  void endArray() override { out += "]"; }
  void stringField(std::string_view key, std::string_view value) override {
    out += std::string(key) + "=" + std::string(value) + ";";
  }
  void boolField(std::string_view key, bool value) override {
    out += std::string(key) + "=" + (value ? "true" : "false") + ";";
  }
  void intField(std::string_view key, int64_t value) override {
    out += std::string(key) + "=" + std::to_string(value) + ";";
  }
  void uintField(std::string_view key, uint64_t value) override {
    out += std::string(key) + "=" + std::to_string(value) + ";";
  }
  void doubleField(std::string_view key, double) override { out += std::string(key) + "=d;"; }
  void nullField(std::string_view key) override { out += std::string(key) + "=null;"; }
  std::string out;
};

TEST_F(PluginConfigTest, SerializerRedactsSecrets) {
  auto res =
      resolve("", "    plugins:\n      - type: test\n        config: {secret: hunter2, tag: 4}\n");
  ASSERT_TRUE(res.hasValue()) << res.error();
  RecordingSink sink;
  config::serializeConfig(res->config, sink);
  EXPECT_THAT(sink.out, HasSubstr("type=test;id=test;rank=0;{config:"));
  EXPECT_THAT(sink.out, HasSubstr("secret=<redacted>;"));
  EXPECT_THAT(sink.out, HasSubstr("tag=4;"));
  EXPECT_THAT(sink.out, ::testing::Not(HasSubstr("hunter2")));
  // The live config keeps the secret.
  const auto& svc = res->config.services.at("live");
  EXPECT_EQ(static_cast<const TestConfig*>(svc.plugins[0].config.get())->secret, "hunter2");
}

} // namespace
} // namespace openmoq::moqx::plugin::test
