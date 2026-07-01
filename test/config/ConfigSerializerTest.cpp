/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "config/ConfigSerializer.h"

#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <rfl/internal/num_fields.hpp>

// Drift guard: rfl reflects the field count of each resolved Config struct. If
// you add or remove a field in Config.h, the matching assert fails and you must
// bump the count *and* update serializeConfig() in ConfigSerializer.h. This is
// the single tripwire that keeps the serializer in sync with the config types.
//
// rfl lives only in the loader/test targets (moqx_config stays rfl-free), so
// the check lives here rather than next to the serializer.
namespace {
using namespace openmoq::moqx::config;

static_assert(rfl::internal::num_fields<Config> == 9, "Config changed — update serializeConfig()");
static_assert(
    rfl::internal::num_fields<ListenerConfig> == 8,
    "ListenerConfig changed — update serializeConfig()"
);
static_assert(
    rfl::internal::num_fields<TlsConfig> == 3,
    "TlsConfig changed — update serializeTls()"
);
static_assert(
    rfl::internal::num_fields<QuicConfig> == 10,
    "QuicConfig changed — update serializeQuic()"
);
static_assert(
    rfl::internal::num_fields<MvfstConfig> == 14,
    "MvfstConfig changed — update serializeMvfst()"
);
static_assert(
    rfl::internal::num_fields<MvfstConfig::BBR> == 5,
    "MvfstConfig::BBR changed — update serializeMvfst()"
);
static_assert(
    rfl::internal::num_fields<MvfstConfig::BBR2> == 11,
    "MvfstConfig::BBR2 changed — update serializeMvfst()"
);
static_assert(
    rfl::internal::num_fields<MvfstConfig::Cubic> == 3,
    "MvfstConfig::Cubic changed — update serializeMvfst()"
);
static_assert(
    rfl::internal::num_fields<MvfstConfig::Copa> == 1,
    "MvfstConfig::Copa changed — update serializeMvfst()"
);
static_assert(
    rfl::internal::num_fields<MvfstConfig::L4S> == 1,
    "MvfstConfig::L4S changed — update serializeMvfst()"
);
static_assert(
    rfl::internal::num_fields<ServiceConfig> == 4,
    "ServiceConfig changed — update serializeConfig()"
);
static_assert(
    rfl::internal::num_fields<ServiceConfig::MatchEntry> == 2,
    "MatchEntry changed — update serializeMatch()"
);
static_assert(
    rfl::internal::num_fields<CacheConfig> == 6,
    "CacheConfig changed — update serializeCache()"
);
static_assert(
    rfl::internal::num_fields<AuthConfig> == 6,
    "AuthConfig changed — update serializeAuth()"
);
static_assert(
    rfl::internal::num_fields<AuthConfig::HmacKey> == 2,
    "HmacKey changed — update serializeAuth()"
);
static_assert(
    rfl::internal::num_fields<UpstreamConfig> == 4,
    "UpstreamConfig changed — update serializeUpstream()"
);
static_assert(
    rfl::internal::num_fields<UpstreamTlsConfig> == 2,
    "UpstreamTlsConfig changed — update serializeUpstream()"
);
static_assert(
    rfl::internal::num_fields<AdminConfig> == 2,
    "AdminConfig changed — update serializeConfig()"
);
static_assert(
    rfl::internal::num_fields<LoggingConfig> == 2,
    "LoggingConfig changed — update serializeLogging()"
);
static_assert(
    rfl::internal::num_fields<MLogConfig> == 2,
    "MLogConfig changed — update serializeLogging()"
);
static_assert(
    rfl::internal::num_fields<QLogConfig> == 2,
    "QLogConfig changed — update serializeLogging()"
);

// Sink that records every emitted scalar leaf as a flat path -> value map, so
// tests can assert on both structure (paths visited) and values.
class RecordingSink : public ConfigSink {
public:
  std::map<std::string, std::string> scalars; // path -> rendered value
  std::vector<std::string> keyStack;

  std::string path(std::string_view key) const {
    std::string p;
    for (const auto& k : keyStack) {
      if (k.empty()) {
        continue; // root scope contributes no segment
      }
      p += k;
      p += '.';
    }
    p += key.empty() ? "[]" : std::string(key);
    return p;
  }

  void beginObject(std::string_view key) override { keyStack.push_back(segment(key)); }
  void endObject() override { keyStack.pop_back(); }
  void beginArray(std::string_view key) override { keyStack.push_back(segment(key)); }
  void endArray() override { keyStack.pop_back(); }

  void stringField(std::string_view key, std::string_view value) override {
    scalars[path(key)] = std::string(value);
  }
  void boolField(std::string_view key, bool value) override {
    scalars[path(key)] = value ? "true" : "false";
  }
  void intField(std::string_view key, int64_t value) override {
    scalars[path(key)] = std::to_string(value);
  }
  void uintField(std::string_view key, uint64_t value) override {
    scalars[path(key)] = std::to_string(value);
  }
  void doubleField(std::string_view key, double value) override {
    scalars[path(key)] = std::to_string(value);
  }
  void nullField(std::string_view key) override { scalars[path(key)] = "null"; }

private:
  // Root object (empty key, empty stack) adds no path segment; an empty key
  // deeper in the walk marks an array element, rendered as "*".
  std::string segment(std::string_view key) const {
    if (keyStack.empty()) {
      return "";
    }
    return key.empty() ? std::string("*") : std::string(key);
  }
};

Config makeFullConfig() {
  Config cfg;
  cfg.relayID = "test-relay";
  cfg.threads = 4;
  cfg.useRelayThread = true;
  cfg.useLocalForwarders = false;
  cfg.mvfstBpfSteering = true;

  cfg.admin = AdminConfig{};
  cfg.admin->address = folly::SocketAddress("::1", 9999);
  cfg.admin->tls = TlsConfig{"/etc/admin.crt", "/etc/admin.key", {}};

  ListenerConfig l;
  l.name = "main";
  l.address = folly::SocketAddress("::", 4433);
  l.tlsMode = TlsConfig{"/etc/relay.crt", "/etc/relay.key", {}};
  l.endpoint = "/moq-relay";
  l.moqtVersions = "16";
  l.quicStack = QuicStack::Mvfst;
  cfg.listeners.push_back(l);

  ServiceConfig svc;
  svc.match.push_back(
      {ServiceConfig::MatchEntry::AnyAuthority{}, ServiceConfig::MatchEntry::PrefixPath{"/"}}
  );
  svc.cache.maxCachedTracks = 100;
  svc.cache.maxCachedGroupsPerTrack = 3;
  svc.auth.enabled = true;
  svc.auth.hmacKeys.push_back({"key-1", "super-secret-value"});
  svc.upstream = UpstreamConfig{};
  svc.upstream->url = "https://upstream.example/relay";
  svc.upstream->tls.caCertFile = "/etc/ca.pem";
  cfg.services.emplace("default", std::move(svc));

  cfg.logging = LoggingConfig{};
  cfg.logging->mlog = MLogConfig{"/var/log/mlog", 0.5f};
  cfg.logging->qlog = QLogConfig{"/var/log/qlog", 1.0f};

  return cfg;
}

TEST(ConfigSerializerTest, VisitsAllSections) {
  Config cfg = makeFullConfig();
  RecordingSink sink;
  serializeConfig(cfg, sink);

  EXPECT_EQ(sink.scalars["relay_id"], "test-relay");
  EXPECT_EQ(sink.scalars["threads"], "4");
  EXPECT_EQ(sink.scalars["admin.address"], "[::1]:9999");
  EXPECT_EQ(sink.scalars["admin.tls.cert_file"], "/etc/admin.crt");

  EXPECT_EQ(sink.scalars["listeners.*.name"], "main");
  EXPECT_EQ(sink.scalars["listeners.*.quic_stack"], "mvfst");
  EXPECT_EQ(sink.scalars["listeners.*.tls.key_file"], "/etc/relay.key");
  EXPECT_EQ(sink.scalars["listeners.*.quic.cc_algo"], "bbr");
  EXPECT_EQ(sink.scalars["listeners.*.mvfst.bbr2.exit_startup_on_loss"], "true");

  EXPECT_EQ(sink.scalars["services.default.cache.enabled"], "true");
  EXPECT_EQ(sink.scalars["services.default.cache.max_cached_tracks"], "100");
  EXPECT_EQ(sink.scalars["services.default.match.*.path.prefix"], "/");
  EXPECT_EQ(sink.scalars["services.default.upstream.url"], "https://upstream.example/relay");
  EXPECT_EQ(sink.scalars["services.default.upstream.tls.ca_cert_file"], "/etc/ca.pem");

  EXPECT_EQ(sink.scalars["logging.mlog.dir"], "/var/log/mlog");
  EXPECT_EQ(sink.scalars["logging.mlog.sample_rate"], "0.500000");
  EXPECT_EQ(sink.scalars["logging.qlog.dir"], "/var/log/qlog");
}

TEST(ConfigSerializerTest, RedactsHmacSecret) {
  Config cfg = makeFullConfig();
  RecordingSink sink;
  serializeConfig(cfg, sink);

  EXPECT_EQ(sink.scalars["services.default.auth.hmac_keys.*.id"], "key-1");
  EXPECT_EQ(sink.scalars["services.default.auth.hmac_keys.*.secret"], "<redacted>");
  for (const auto& [_, value] : sink.scalars) {
    EXPECT_NE(value, "super-secret-value") << "secret leaked into serialized config";
  }
}

} // namespace
