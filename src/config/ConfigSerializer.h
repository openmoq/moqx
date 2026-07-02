/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <string_view>
#include <variant>

#include "config/Config.h"

// Format-agnostic walk over a resolved Config. The walker lives next to the
// Config types (no JSON/YAML dependency) so the field list has a single home;
// callers supply a ConfigSink that renders the callbacks however they like
// (e.g. admin/ConfigHandler.cpp emits JSON).
//
// IMPORTANT: when you add a field to Config.h, add it here too. The field-count
// static_asserts in test/config/ConfigSerializerTest.cpp fail the build if you
// forget. See docs/dev/config.md ("How to add a new config field").
namespace openmoq::moqx::config {

// Sink callbacks. An empty `key` means the value is an array element (rendered
// without a key); a non-empty `key` means it is a named object field.
class ConfigSink {
public:
  virtual ~ConfigSink() = default;

  virtual void beginObject(std::string_view key) = 0;
  virtual void endObject() = 0;
  virtual void beginArray(std::string_view key) = 0;
  virtual void endArray() = 0;

  virtual void stringField(std::string_view key, std::string_view value) = 0;
  virtual void boolField(std::string_view key, bool value) = 0;
  virtual void intField(std::string_view key, int64_t value) = 0;
  virtual void uintField(std::string_view key, uint64_t value) = 0;
  virtual void doubleField(std::string_view key, double value) = 0;
  virtual void nullField(std::string_view key) = 0;
};

namespace detail {

inline std::string_view quicStackName(QuicStack s) {
  switch (s) {
  case QuicStack::Mvfst:
    return "mvfst";
  case QuicStack::Picoquic:
    return "picoquic";
  case QuicStack::ProxygenQmux:
    return "proxygen_qmux";
  }
  return "unknown";
}

inline void serializeTls(ConfigSink& s, const TlsConfig& tls) {
  s.boolField("insecure", false);
  s.stringField("cert_file", tls.certFile);
  s.stringField("key_file", tls.keyFile);
  // PKCS#12-sourced material holds the decrypted private key in memory. Never
  // serialize it into the /config dump — expose only whether it is present.
  s.boolField("pkcs12_in_memory", tls.material.has_value());
  s.beginArray("alpn");
  for (const auto& a : tls.alpn) {
    s.stringField("", a);
  }
  s.endArray();
}

inline void serializeListenerTls(ConfigSink& s, const TlsMode& mode) {
  s.beginObject("tls");
  if (const auto* tls = std::get_if<TlsConfig>(&mode)) {
    serializeTls(s, *tls);
  } else {
    s.boolField("insecure", true);
  }
  s.endObject();
}

inline void serializeQuic(ConfigSink& s, const QuicConfig& q) {
  s.beginObject("quic");
  s.uintField("max_data", q.maxData);
  s.uintField("max_stream_data", q.maxStreamData);
  s.uintField("max_uni_streams", q.maxUniStreams);
  s.uintField("max_bidi_streams", q.maxBidiStreams);
  s.uintField("idle_timeout_ms", q.idleTimeoutMs);
  s.uintField("max_ack_delay_us", q.maxAckDelayUs);
  s.uintField("min_ack_delay_us", q.minAckDelayUs);
  s.uintField("default_stream_priority", q.defaultStreamPriority);
  s.uintField("default_datagram_priority", q.defaultDatagramPriority);
  s.stringField("cc_algo", q.ccAlgo);
  s.endObject();
}

inline void serializeMvfst(ConfigSink& s, const MvfstConfig& m) {
  s.beginObject("mvfst");
  s.uintField("max_cwnd_in_mss", m.maxCwndInMss);
  s.boolField("pacing_enabled", m.pacingEnabled);
  s.boolField("enable_gso", m.enableGSO);
  s.uintField("max_conn_packets_sent_per_loop", m.maxConnPacketsSentPerLoop);
  s.boolField("use_recvmmsg", m.useRecvmmsg);
  s.uintField("max_server_recv_packets_per_loop", m.maxServerRecvPacketsPerLoop);
  s.uintField("num_gro_buffers", m.numGROBuffers);
  s.boolField("can_ignore_path_mtu", m.canIgnorePathMTU);
  s.uintField("udp_socket_buffer_bytes", m.udpSocketBufferBytes);

  s.beginObject("bbr");
  s.boolField("conservative_recovery", m.bbr.conservativeRecovery);
  s.boolField("large_probe_rtt_cwnd", m.bbr.largeProbeRttCwnd);
  s.boolField("enable_ack_aggregation_in_startup", m.bbr.enableAckAggregationInStartup);
  s.boolField("probe_rtt_disabled_if_app_limited", m.bbr.probeRttDisabledIfAppLimited);
  s.boolField("drain_to_target", m.bbr.drainToTarget);
  s.endObject();

  s.beginObject("bbr2");
  s.boolField("ignore_inflight_long_term", m.bbr2.ignoreInflightLongTerm);
  s.boolField("ignore_short_term", m.bbr2.ignoreShortTerm);
  s.boolField("exit_startup_on_loss", m.bbr2.exitStartupOnLoss);
  s.boolField("enable_recovery_in_startup", m.bbr2.enableRecoveryInStartup);
  s.boolField("enable_recovery_in_probe_states", m.bbr2.enableRecoveryInProbeStates);
  s.boolField("enable_reno_coexistence", m.bbr2.enableRenoCoexistence);
  s.boolField("pace_init_cwnd", m.bbr2.paceInitCwnd);
  s.doubleField("override_cruise_pacing_gain", m.bbr2.overrideCruisePacingGain);
  s.doubleField("override_cruise_cwnd_gain", m.bbr2.overrideCruiseCwndGain);
  s.doubleField("override_startup_pacing_gain", m.bbr2.overrideStartupPacingGain);
  s.doubleField("override_bw_short_beta", m.bbr2.overrideBwShortBeta);
  s.endObject();

  s.beginObject("cubic");
  s.boolField("additive_increase_after_hystart", m.cubic.additiveIncreaseAfterHystart);
  s.boolField("only_grow_cwnd_when_limited", m.cubic.onlyGrowCwndWhenLimited);
  s.boolField("leave_headroom_for_cwnd_limited", m.cubic.leaveHeadroomForCwndLimited);
  s.endObject();

  s.beginObject("copa");
  s.doubleField("delta_param", m.copa.deltaParam);
  s.endObject();

  s.beginObject("l4s");
  s.doubleField("ce_target", m.l4s.ceTarget);
  s.endObject();

  s.endObject();
}

inline void serializeMatch(ConfigSink& s, const ServiceConfig::MatchEntry& m) {
  s.beginObject("");
  s.beginObject("authority");
  if (const auto* e = std::get_if<ServiceConfig::MatchEntry::ExactAuthority>(&m.authority)) {
    s.stringField("exact", e->value);
  } else if (const auto* p =
                 std::get_if<ServiceConfig::MatchEntry::WildcardAuthority>(&m.authority)) {
    s.stringField("wildcard", p->pattern);
  } else {
    s.boolField("any", true);
  }
  s.endObject();
  s.beginObject("path");
  if (const auto* e = std::get_if<ServiceConfig::MatchEntry::ExactPath>(&m.path)) {
    s.stringField("exact", e->value);
  } else if (const auto* p = std::get_if<ServiceConfig::MatchEntry::PrefixPath>(&m.path)) {
    s.stringField("prefix", p->value);
  }
  s.endObject();
  s.endObject();
}

inline void serializeCache(ConfigSink& s, const CacheConfig& c) {
  s.beginObject("cache");
  s.boolField("enabled", c.maxCachedTracks != 0);
  s.uintField("max_cached_tracks", c.maxCachedTracks);
  s.uintField("max_cached_groups_per_track", c.maxCachedGroupsPerTrack);
  s.uintField("max_cached_mb", c.maxCachedMb);
  s.uintField("min_eviction_kb", c.minEvictionKb);
  s.intField("max_cache_duration_ms", c.maxCacheDuration.count());
  if (c.defaultMaxCacheDuration) {
    s.intField("default_max_cache_duration_ms", c.defaultMaxCacheDuration->count());
  } else {
    s.nullField("default_max_cache_duration_ms");
  }
  s.endObject();
}

inline void serializeAuth(ConfigSink& s, const AuthConfig& a) {
  s.beginObject("auth");
  s.boolField("enabled", a.enabled);
  s.uintField("token_type", a.tokenType);
  s.boolField("require_setup_token", a.requireSetupToken);
  s.boolField("allow_request_token_override", a.allowRequestTokenOverride);
  s.boolField("strict_claims", a.strictClaims);
  s.beginArray("hmac_keys");
  for (const auto& k : a.hmacKeys) {
    s.beginObject("");
    s.stringField("id", k.id);
    // Signing secrets are never serialized — redacted at the source so no sink
    // can leak them (e.g. over the /config admin endpoint).
    s.stringField("secret", "<redacted>");
    s.endObject();
  }
  s.endArray();
  s.endObject();
}

inline void serializeLogging(ConfigSink& s, const LoggingConfig& l) {
  s.beginObject("logging");
  if (l.mlog) {
    s.beginObject("mlog");
    s.stringField("dir", l.mlog->dir);
    s.doubleField("sample_rate", l.mlog->sampleRate);
    s.endObject();
  } else {
    s.nullField("mlog");
  }
  if (l.qlog) {
    s.beginObject("qlog");
    s.stringField("dir", l.qlog->dir);
    s.doubleField("sample_rate", l.qlog->sampleRate);
    s.endObject();
  } else {
    s.nullField("qlog");
  }
  s.endObject();
}

inline void serializeUpstream(ConfigSink& s, const UpstreamConfig& u) {
  s.beginObject("upstream");
  s.stringField("url", u.url);
  s.beginObject("tls");
  s.boolField("insecure", u.tls.insecure);
  if (u.tls.caCertFile) {
    s.stringField("ca_cert_file", *u.tls.caCertFile);
  } else {
    s.nullField("ca_cert_file");
  }
  s.endObject();
  s.intField("connect_timeout_ms", u.connectTimeout.count());
  s.intField("idle_timeout_ms", u.idleTimeout.count());
  s.endObject();
}

} // namespace detail

inline void serializeConfig(const Config& cfg, ConfigSink& s) {
  using namespace detail;

  s.beginObject("");
  s.stringField("relay_id", cfg.relayID);
  s.uintField("threads", cfg.threads);
  s.boolField("use_relay_thread", cfg.useRelayThread);
  s.boolField("use_local_forwarders", cfg.useLocalForwarders);
  s.boolField("mvfst_bpf_steering", cfg.mvfstBpfSteering);

  if (cfg.admin) {
    s.beginObject("admin");
    s.stringField("address", cfg.admin->address.describe());
    if (cfg.admin->tls) {
      s.beginObject("tls");
      serializeTls(s, *cfg.admin->tls);
      s.endObject();
    } else {
      s.nullField("tls");
    }
    s.endObject();
  } else {
    s.nullField("admin");
  }

  s.beginArray("listeners");
  for (const auto& l : cfg.listeners) {
    s.beginObject("");
    s.stringField("name", l.name);
    s.stringField("address", l.address.describe());
    s.stringField("endpoint", l.endpoint);
    s.stringField("moqt_versions", l.moqtVersions);
    s.stringField("quic_stack", quicStackName(l.quicStack));
    serializeListenerTls(s, l.tlsMode);
    serializeQuic(s, l.quic);
    serializeMvfst(s, l.mvfst);
    s.endObject();
  }
  s.endArray();

  s.beginObject("services");
  for (const auto& [name, svc] : cfg.services) {
    s.beginObject(name);
    s.beginArray("match");
    for (const auto& m : svc.match) {
      serializeMatch(s, m);
    }
    s.endArray();
    serializeCache(s, svc.cache);
    serializeAuth(s, svc.auth);
    if (svc.upstream) {
      serializeUpstream(s, *svc.upstream);
    }
    s.endObject();
  }
  s.endObject();

  if (cfg.logging) {
    serializeLogging(s, *cfg.logging);
  } else {
    s.nullField("logging");
  }

  s.endObject();
}

} // namespace openmoq::moqx::config
