/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "config/loader/ConfigResolver.h"

#include <iomanip>
#include <random>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <folly/String.h>

namespace openmoq::moqx::config {

namespace {

constexpr const char* kStackMvfst = "mvfst";
constexpr const char* kStackPicoquic = "picoquic";

// Format a label for error messages: "Service 'name' match[j]"
std::string matchRuleErrorLabel(const std::string& name, size_t j) {
  return "Service '" + name + "' match[" + std::to_string(j) + "]";
}

std::string moqtVersionsToString(const ParsedListenerConfig& listener) {
  if (!listener.moqt_versions.value().has_value() || listener.moqt_versions.value()->empty()) {
    return "";
  }
  return folly::join(',', *listener.moqt_versions.value());
}

// Merge two cache configs: overlay takes precedence over base, field by field.
ParsedCacheConfig
mergeCacheConfigs(const ParsedCacheConfig& base, const ParsedCacheConfig& overlay) {
  ParsedCacheConfig merged;
  merged.enabled =
      overlay.enabled.value().has_value() ? overlay.enabled.value() : base.enabled.value();
  merged.max_tracks =
      overlay.max_tracks.value().has_value() ? overlay.max_tracks.value() : base.max_tracks.value();
  merged.max_groups_per_track = overlay.max_groups_per_track.value().has_value()
                                    ? overlay.max_groups_per_track.value()
                                    : base.max_groups_per_track.value();
  merged.max_cached_mb = overlay.max_cached_mb.value().has_value() ? overlay.max_cached_mb.value()
                                                                   : base.max_cached_mb.value();
  merged.min_eviction_kb = overlay.min_eviction_kb.value().has_value()
                               ? overlay.min_eviction_kb.value()
                               : base.min_eviction_kb.value();
  merged.max_cache_duration_s = overlay.max_cache_duration_s.value().has_value()
                                    ? overlay.max_cache_duration_s.value()
                                    : base.max_cache_duration_s.value();
  merged.default_max_cache_duration_s = overlay.default_max_cache_duration_s.value().has_value()
                                            ? overlay.default_max_cache_duration_s.value()
                                            : base.default_max_cache_duration_s.value();
  return merged;
}

void validateListenerTlsConfig(
    const ParsedListenerTlsConfig& tls,
    std::string_view context,
    std::vector<std::string>& errors,
    std::vector<std::string>& warnings
) {
  bool hasCert = tls.cert_file.value().has_value() && !tls.cert_file.value()->empty();
  bool hasKey = tls.key_file.value().has_value() && !tls.key_file.value()->empty();
  if (!tls.insecure.value()) {
    if (!hasCert || !hasKey) {
      errors.push_back(
          std::string(context) + ": cert_file and key_file are required when insecure=false"
      );
    }
  } else if (hasCert || hasKey) {
    warnings.push_back(
        std::string(context) + ": cert_file/key_file are ignored when insecure=true"
    );
  }
}

void validateAdminTlsConfig(const ParsedAdminTlsConfig& tls, std::vector<std::string>& errors) {
  bool hasCert = tls.cert_file.value().has_value() && !tls.cert_file.value()->empty();
  bool hasKey = tls.key_file.value().has_value() && !tls.key_file.value()->empty();
  if (!hasCert || !hasKey) {
    errors.push_back("admin.tls: cert_file and key_file are required");
  }
}

TlsConfig resolveTlsConfig(const ParsedListenerTlsConfig& tls) {
  return TlsConfig{
      .certFile = tls.cert_file.value().value_or(""),
      .keyFile = tls.key_file.value().value_or(""),
      .alpn = {},
  };
}

TlsConfig resolveAdminTlsConfig(
    const ParsedAdminTlsConfig& tls,
    const std::vector<std::string>& defaultAlpn
) {
  return TlsConfig{
      .certFile = tls.cert_file.value().value_or(""),
      .keyFile = tls.key_file.value().value_or(""),
      .alpn = tls.alpn.value().value_or(defaultAlpn),
  };
}

CacheConfig resolveCacheConfig(const ParsedCacheConfig& cache) {
  // All fields must be present after merging (validated earlier).
  // max_cache_duration_s: absent → 1 day default.
  constexpr uint32_t kDefaultMaxCacheDurationS = 86400;
  const std::chrono::milliseconds maxCacheDuration =
      std::chrono::seconds(cache.max_cache_duration_s.value().value_or(kDefaultMaxCacheDurationS));
  // default_max_cache_duration_s: absent → use maxCacheDuration; 0 → 0ms (opt-in only);
  // N → N seconds for tracks without a publisher-set cache duration.
  std::optional<std::chrono::milliseconds> defaultMaxCacheDuration;
  const auto& defaultDurationOpt = cache.default_max_cache_duration_s.value();
  if (defaultDurationOpt.has_value()) {
    defaultMaxCacheDuration = std::chrono::seconds(*defaultDurationOpt);
  } else {
    defaultMaxCacheDuration = maxCacheDuration;
  }
  return CacheConfig{
      .maxCachedTracks =
          *cache.enabled.value() ? static_cast<size_t>(*cache.max_tracks.value()) : 0,
      .maxCachedGroupsPerTrack = static_cast<size_t>(*cache.max_groups_per_track.value()),
      .maxCachedMb = cache.max_cached_mb.value().value_or(16),
      .minEvictionKb = cache.min_eviction_kb.value().value_or(64),
      .maxCacheDuration = maxCacheDuration,
      .defaultMaxCacheDuration = defaultMaxCacheDuration,
  };
}

// Encode an authority + path pair as a composite key for duplicate detection.
std::string makeCompositeKey(
    const std::string& authorityType,
    const std::string& authorityValue,
    const ParsedServiceConfig::MatchRule::PathMatch& path
) {
  std::string key = authorityType + ":" + authorityValue + "|";
  path.visit([&](const auto& alt) {
    using P = std::decay_t<decltype(alt)>;
    if constexpr (std::is_same_v<P, ParsedServiceConfig::MatchRule::ExactPath>) {
      key += "exact:" + alt.exact.value();
    } else {
      key += "prefix:" + alt.prefix.value();
    }
  });
  return key;
}

// --- Listener validation ---

void validateListener(
    const ParsedListenerConfig& listener,
    std::vector<std::string>& errors,
    std::vector<std::string>& warnings
) {
  const auto& sock = listener.udp.value().socket.value();
  if (sock.port.value() == 0) {
    errors.push_back("Listener '" + listener.name.value() + "' port must be 1-65535, got 0");
  }

  // TLS validation
  validateListenerTlsConfig(
      listener.tls.value(),
      "Listener '" + listener.name.value() + "'",
      errors,
      warnings
  );

  // quic_stack validation
  const auto& stackOpt = listener.quic_stack.value();
  if (stackOpt.has_value() && *stackOpt != kStackMvfst && *stackOpt != kStackPicoquic) {
    errors.push_back(
        "Listener '" + listener.name.value() + "': unknown quic_stack '" + *stackOpt +
        "' (expected \"mvfst\" or \"picoquic\")"
    );
  }
  if (stackOpt.value_or(kStackMvfst) == kStackPicoquic && listener.tls.value().insecure.value()) {
    errors.push_back(
        "Listener '" + listener.name.value() +
        "': quic_stack \"picoquic\" requires real TLS credentials (insecure: true is not supported)"
    );
  }
}

// --- Admin validation ---

void validateAdmin(const ParsedConfig& config, std::vector<std::string>& errors) {
  const auto& adminOptional = config.admin.value();
  if (adminOptional.has_value()) {
    if (adminOptional->port.value() == 0) {
      errors.push_back("admin.port must be 1-65535, got 0");
    }
    bool hasTls = adminOptional->tls.value().has_value();
    bool hasPlaintext = adminOptional->plaintext.value();
    if (hasTls && hasPlaintext) {
      errors.push_back("admin: plaintext and tls are mutually exclusive");
    } else if (!hasTls && !hasPlaintext) {
      errors.push_back("admin: one of plaintext or tls must be set");
    }
    if (hasTls) {
      validateAdminTlsConfig(*adminOptional->tls.value(), errors);
    }
  }
}

// --- Match-entry validation helpers ---

struct AuthorityInfo {
  std::string type;  // "exact", "wildcard", or "any"
  std::string value; // authority value (empty for "any")
};

// Returns nullopt if the authority is invalid.
std::optional<AuthorityInfo> validateAuthority(
    const ParsedServiceConfig::MatchRule::AuthorityMatch& authority,
    const std::string& prefix,
    std::vector<std::string>& errors
) {
  std::optional<AuthorityInfo> result;

  authority.visit([&](const auto& alt) {
    using A = std::decay_t<decltype(alt)>;
    if constexpr (std::is_same_v<A, ParsedServiceConfig::MatchRule::ExactAuthority>) {
      if (alt.exact.value().empty()) {
        errors.push_back(prefix + ".authority: exact value must be non-empty");
      } else {
        result = AuthorityInfo{"exact", alt.exact.value()};
      }
    } else if constexpr (std::is_same_v<A, ParsedServiceConfig::MatchRule::WildcardAuthority>) {
      const auto& pat = alt.wildcard.value();
      if (pat.size() < 3 || pat[0] != '*' || pat[1] != '.') {
        errors.push_back(
            prefix + ".authority: wildcard must start with '*.' (e.g. '*.example.com'), got '" +
            pat + "'"
        );
      } else if (pat.substr(2).find('*') != std::string::npos) {
        errors.push_back(
            prefix + ".authority: wildcard must contain exactly one '*' at the start, got '" + pat +
            "'"
        );
      } else {
        result = AuthorityInfo{"wildcard", pat};
      }
    } else { // AnyAuthority
      if (!alt.any.value()) {
        errors.push_back(prefix + ".authority: any must be true");
      } else {
        result = AuthorityInfo{"any", ""};
      }
    }
  });

  return result;
}

// Returns false if the path is invalid.
bool validatePath(
    const ParsedServiceConfig::MatchRule::PathMatch& path,
    const std::string& prefix,
    std::vector<std::string>& errors
) {
  bool valid = true;
  path.visit([&](const auto& alt) {
    using P = std::decay_t<decltype(alt)>;
    const std::string& pathVal = [&]() -> const std::string& {
      if constexpr (std::is_same_v<P, ParsedServiceConfig::MatchRule::ExactPath>) {
        return alt.exact.value();
      } else {
        return alt.prefix.value();
      }
    }();

    if (pathVal.empty()) {
      errors.push_back(prefix + ".path: value must be non-empty");
      valid = false;
    } else if (pathVal[0] != '/') {
      errors.push_back(prefix + ".path: value must start with '/', got '" + pathVal + "'");
      valid = false;
    }
  });
  return valid;
}

// --- Upstream validation and resolution ---

void validateUpstream(const ParsedUpstreamConfig& upstream, std::vector<std::string>& errors) {
  if (upstream.url.value().empty()) {
    errors.push_back("upstream.url must be non-empty");
  }
  const auto& tls = upstream.tls.value();
  if (tls.insecure.value() && tls.ca_cert.value().has_value()) {
    errors.push_back("upstream.tls: insecure=true and ca_cert are mutually exclusive");
  }
}

UpstreamConfig resolveUpstream(const ParsedUpstreamConfig& upstream) {
  const auto& tls = upstream.tls.value();
  return UpstreamConfig{
      .url = upstream.url.value(),
      .tls =
          UpstreamTlsConfig{
              .insecure = tls.insecure.value(),
              .caCertFile = tls.ca_cert.value(),
          },
      .connectTimeout =
          std::chrono::milliseconds(upstream.connect_timeout_ms.value().value_or(5000)),
      .idleTimeout = std::chrono::milliseconds(upstream.idle_timeout_ms.value().value_or(5000)),
  };
}

// Minimum HMAC-SHA256 shared-secret length. Short secrets undermine the MAC, so
// reject them at config load when auth is enabled.
constexpr size_t kMinHmacSecretBytes = 16;

// MOQT carries token_type as a QUIC variable-length integer (RFC 9000 Section
// 16), whose maximum encodable value is 2^62 - 1. This bound is a property of
// the wire encoding, not a moqx policy, so it is stable across MOQT drafts: a
// value at or above 2^62 simply cannot be serialized as a varint.
constexpr uint64_t kQuicVarintExclusiveUpperBound = uint64_t{1} << 62;

void validateAuth(
    const std::string& serviceName,
    const ParsedAuthConfig& auth,
    std::vector<std::string>& errors
) {
  if (!auth.enabled.value()) {
    return;
  }
  const auto& keys = auth.hmac_keys.value();
  if (!keys.has_value() || keys->empty()) {
    errors.push_back(
        "Service '" + serviceName + "': auth.hmac_keys is required when auth is enabled"
    );
  } else {
    std::unordered_set<std::string> keyIDs;
    for (size_t i = 0; i < keys->size(); ++i) {
      const auto& key = (*keys)[i];
      const auto prefix =
          "Service '" + serviceName + "': auth.hmac_keys[" + std::to_string(i) + "]";
      if (key.id.value().empty()) {
        errors.push_back(prefix + ".id must be non-empty");
      } else if (!keyIDs.insert(key.id.value()).second) {
        errors.push_back(prefix + ".id duplicates another auth key");
      }
      if (key.secret.value().empty()) {
        errors.push_back(prefix + ".secret must be non-empty");
      }
    }
  }
  // token_type 0 is a valid MOQT AUTHORIZATION_TOKEN type and is accepted as-is;
  // it is not treated as "unset". An omitted token_type also resolves to 0 (see
  // resolveAuth), so omitting it and setting token_type: 0 behave identically.
  if (auth.token_type.value().value_or(0) >= kQuicVarintExclusiveUpperBound) {
    errors.push_back("Service '" + serviceName + "': auth.token_type must fit in a QUIC varint");
  }
}

AuthConfig resolveAuth(const std::optional<ParsedAuthConfig>& parsed) {
  AuthConfig out;
  if (!parsed) {
    return out;
  }
  out.enabled = parsed->enabled.value();
  out.tokenType = parsed->token_type.value().value_or(0);
  out.requireSetupToken = parsed->require_setup_token.value().value_or(true);
  out.allowRequestTokenOverride = parsed->allow_request_token_override.value().value_or(true);
  out.strictClaims = parsed->strict_claims.value().value_or(false);
  if (parsed->hmac_keys.value()) {
    for (const auto& key : *parsed->hmac_keys.value()) {
      out.hmacKeys.push_back(AuthConfig::HmacKey{
          .id = key.id.value(),
          .secret = key.secret.value(),
      });
    }
  }
  return out;
}

// --- Service validation ---

void validateService(
    const std::string& name,
    const ParsedServiceConfig& svc,
    const ParsedConfig& config,
    std::unordered_set<std::string>& compositeKeys,
    std::unordered_map<std::string, ParsedCacheConfig>& mergedCaches,
    std::vector<std::string>& errors
) {

  // Validate each match entry
  const auto& matchEntries = svc.match.value();
  for (size_t j = 0; j < matchEntries.size(); ++j) {
    const auto& entry = matchEntries[j];
    auto prefix = matchRuleErrorLabel(name, j);

    auto authInfo = validateAuthority(entry.authority.value(), prefix, errors);
    if (!authInfo) {
      continue;
    }

    if (!validatePath(entry.path.value(), prefix, errors)) {
      continue;
    }

    auto key = makeCompositeKey(authInfo->type, authInfo->value, entry.path.value());
    if (!compositeKeys.insert(key).second) {
      errors.push_back("Duplicate (authority, path) combination in service '" + name + "': " + key);
    }
  }

  // Cache resolution: merge service_defaults.cache with service.cache (field by field).
  const ParsedCacheConfig* defaults = nullptr;
  if (config.service_defaults.value().has_value() &&
      config.service_defaults.value()->cache.value().has_value()) {
    defaults = &(*config.service_defaults.value()->cache.value());
  }

  ParsedCacheConfig merged;
  if (svc.cache.value().has_value() && defaults) {
    merged = mergeCacheConfigs(*defaults, *svc.cache.value());
  } else if (svc.cache.value().has_value()) {
    merged = *svc.cache.value();
  } else if (defaults) {
    merged = *defaults;
  }
  // else: merged has all-nullopt fields

  // Validate that all fields are present after merging.
  if (!merged.enabled.value().has_value()) {
    errors.push_back("Service '" + name + "': cache.enabled is required");
  }
  if (!merged.max_tracks.value().has_value()) {
    errors.push_back("Service '" + name + "': cache.max_tracks is required");
  }
  if (!merged.max_groups_per_track.value().has_value()) {
    errors.push_back("Service '" + name + "': cache.max_groups_per_track is required");
  }

  if (merged.enabled.value().has_value() && *merged.enabled.value() &&
      merged.max_groups_per_track.value().has_value() && *merged.max_groups_per_track.value() < 1) {
    errors.push_back(
        "Service '" + name + "': cache.max_groups_per_track must be >= 1 when cache is enabled"
    );
  }
  if (merged.max_cache_duration_s.value().has_value() &&
      *merged.max_cache_duration_s.value() == 0) {
    errors.push_back("Service '" + name + "': cache.max_cache_duration_s must not be 0");
  }
  if (merged.max_cached_mb.value().has_value() && *merged.max_cached_mb.value() == 0) {
    errors.push_back("Service '" + name + "': cache.max_cached_mb must not be 0");
  }

  mergedCaches.emplace(name, std::move(merged));

  // Validate per-service upstream if present.
  if (svc.upstream.value().has_value()) {
    validateUpstream(*svc.upstream.value(), errors);
  }
  if (svc.auth.value().has_value()) {
    validateAuth(name, *svc.auth.value(), errors);
  }
}

std::string generateRelayID() {
  std::random_device rd;
  const uint64_t val = (static_cast<uint64_t>(rd()) << 32) | static_cast<uint64_t>(rd());
  std::ostringstream oss;
  oss << std::hex << std::setfill('0') << std::setw(16) << val;
  return oss.str();
}

// --- Resolution helpers ---

// Apply parsed quic fields onto a QuicConfig, field by field.
void applyQuicOverride(QuicConfig& base, const ParsedQuicConfig& overlay) {
  if (auto v = overlay.max_data.value())
    base.maxData = *v;
  if (auto v = overlay.max_stream_data.value())
    base.maxStreamData = *v;
  if (auto v = overlay.max_uni_streams.value())
    base.maxUniStreams = *v;
  if (auto v = overlay.max_bidi_streams.value())
    base.maxBidiStreams = *v;
  if (auto v = overlay.idle_timeout_ms.value())
    base.idleTimeoutMs = *v;
  if (auto v = overlay.max_ack_delay_us.value())
    base.maxAckDelayUs = *v;
  if (auto v = overlay.min_ack_delay_us.value())
    base.minAckDelayUs = *v;
  if (auto v = overlay.default_stream_priority.value())
    base.defaultStreamPriority = *v;
  if (auto v = overlay.default_datagram_priority.value())
    base.defaultDatagramPriority = *v;
  if (auto v = overlay.cc_algo.value())
    base.ccAlgo = *v;
}

// Merge listener_defaults.quic and per-listener quic override into a resolved QuicConfig.
QuicConfig mergeQuicConfig(
    const std::optional<ParsedQuicConfig>& defaults,
    const std::optional<ParsedQuicConfig>& perListener
) {
  QuicConfig result; // starts with C++ struct defaults
  if (defaults)
    applyQuicOverride(result, *defaults);
  if (perListener)
    applyQuicOverride(result, *perListener);
  return result;
}

void applyMvfstOverride(MvfstConfig& base, const ParsedMvfstConfig& overlay) {
  if (auto v = overlay.max_cwnd_in_mss.value())
    base.maxCwndInMss = *v;
  if (auto v = overlay.pacing_enabled.value())
    base.pacingEnabled = *v;
  if (auto v = overlay.enable_gso.value())
    base.enableGSO = *v;
  if (auto v = overlay.max_conn_packets_sent_per_loop.value())
    base.maxConnPacketsSentPerLoop = *v;
  if (auto v = overlay.use_recvmmsg.value())
    base.useRecvmmsg = *v;
  if (auto v = overlay.max_server_recv_packets_per_loop.value())
    base.maxServerRecvPacketsPerLoop = *v;
  if (auto v = overlay.num_gro_buffers.value())
    base.numGROBuffers = *v;
  if (const auto& b = overlay.bbr.value()) {
    if (auto v = b->conservative_recovery.value())
      base.bbr.conservativeRecovery = *v;
    if (auto v = b->large_probe_rtt_cwnd.value())
      base.bbr.largeProbeRttCwnd = *v;
    if (auto v = b->enable_ack_aggregation_in_startup.value())
      base.bbr.enableAckAggregationInStartup = *v;
    if (auto v = b->probe_rtt_disabled_if_app_limited.value())
      base.bbr.probeRttDisabledIfAppLimited = *v;
    if (auto v = b->drain_to_target.value())
      base.bbr.drainToTarget = *v;
  }
  if (const auto& b2 = overlay.bbr2.value()) {
    if (auto v = b2->ignore_inflight_long_term.value())
      base.bbr2.ignoreInflightLongTerm = *v;
    if (auto v = b2->ignore_short_term.value())
      base.bbr2.ignoreShortTerm = *v;
    if (auto v = b2->exit_startup_on_loss.value())
      base.bbr2.exitStartupOnLoss = *v;
    if (auto v = b2->enable_recovery_in_startup.value())
      base.bbr2.enableRecoveryInStartup = *v;
    if (auto v = b2->enable_recovery_in_probe_states.value())
      base.bbr2.enableRecoveryInProbeStates = *v;
    if (auto v = b2->enable_reno_coexistence.value())
      base.bbr2.enableRenoCoexistence = *v;
    if (auto v = b2->pace_init_cwnd.value())
      base.bbr2.paceInitCwnd = *v;
    if (auto v = b2->override_cruise_pacing_gain.value())
      base.bbr2.overrideCruisePacingGain = *v;
    if (auto v = b2->override_cruise_cwnd_gain.value())
      base.bbr2.overrideCruiseCwndGain = *v;
    if (auto v = b2->override_startup_pacing_gain.value())
      base.bbr2.overrideStartupPacingGain = *v;
    if (auto v = b2->override_bw_short_beta.value())
      base.bbr2.overrideBwShortBeta = *v;
  }
  if (const auto& c = overlay.cubic.value()) {
    if (auto v = c->additive_increase_after_hystart.value())
      base.cubic.additiveIncreaseAfterHystart = *v;
    if (auto v = c->only_grow_cwnd_when_limited.value())
      base.cubic.onlyGrowCwndWhenLimited = *v;
    if (auto v = c->leave_headroom_for_cwnd_limited.value())
      base.cubic.leaveHeadroomForCwndLimited = *v;
  }
  if (const auto& cp = overlay.copa.value()) {
    if (auto v = cp->delta_param.value())
      base.copa.deltaParam = *v;
  }
  if (const auto& ls = overlay.l4s.value()) {
    if (auto v = ls->ce_target.value())
      base.l4s.ceTarget = *v;
  }
  if (auto v = overlay.udp_socket_buffer_bytes.value())
    base.udpSocketBufferBytes = *v;
}

MvfstConfig mergeMvfstConfig(
    const std::optional<ParsedMvfstConfig>& defaults,
    const std::optional<ParsedMvfstConfig>& perListener
) {
  MvfstConfig result; // starts with C++ struct defaults
  if (defaults)
    applyMvfstOverride(result, *defaults);
  if (perListener)
    applyMvfstOverride(result, *perListener);
  return result;
}

void validateQuicConfig(
    const QuicConfig& quic,
    QuicStack stack,
    const std::string& context,
    std::vector<std::string>& errors,
    std::vector<std::string>& warnings
) {
  if (quic.maxData < quic.maxStreamData) {
    errors.push_back(
        context + " quic: max_data (" + std::to_string(quic.maxData) +
        ") must be >= max_stream_data (" + std::to_string(quic.maxStreamData) + ")"
    );
  }
  if (quic.maxUniStreams < 100) {
    warnings.push_back(
        context + " quic: max_uni_streams (" + std::to_string(quic.maxUniStreams) +
        ") is very low (< 100); this may limit throughput"
    );
  }
  if (quic.maxBidiStreams < 16) {
    warnings.push_back(
        context + " quic: max_bidi_streams (" + std::to_string(quic.maxBidiStreams) +
        ") is very low (< 16)"
    );
  }
  if (quic.idleTimeoutMs < 5000) {
    warnings.push_back(
        context + " quic: idle_timeout_ms (" + std::to_string(quic.idleTimeoutMs) +
        ") is very aggressive (< 5000ms)"
    );
  }
  if (quic.maxAckDelayUs < quic.minAckDelayUs) {
    errors.push_back(
        context + " quic: max_ack_delay_us (" + std::to_string(quic.maxAckDelayUs) +
        ") must be >= min_ack_delay_us (" + std::to_string(quic.minAckDelayUs) + ")"
    );
  }
  // (excluded: mvfst "custom"/"staticcwnd" require programmatic setup)
  static const std::unordered_set<std::string> kPicoCcAlgos =
      {"bbr", "bbr1", "c4", "cubic", "dcubic", "fast", "newreno", "prague", "reno"};
  static const std::unordered_set<std::string> kMvfstCcAlgos =
      {"bbr", "bbr2", "bbr2modular", "copa", "cubic", "newreno", "none"};
  const auto& validAlgos = (stack == QuicStack::Picoquic) ? kPicoCcAlgos : kMvfstCcAlgos;
  const auto& validList = (stack == QuicStack::Picoquic)
                              ? "bbr, bbr1, c4, cubic, dcubic, fast, newreno, prague, reno"
                              : "bbr, bbr2, bbr2modular, copa, cubic, newreno, none";
  if (!validAlgos.count(quic.ccAlgo)) {
    errors.push_back(
        context + " quic: cc_algo '" + quic.ccAlgo +
        "' is not valid for this stack"
        " (valid: " +
        validList + ")"
    );
  }
}

void validateMvfstConfig(
    const MvfstConfig& mvfst,
    const std::string& context,
    std::vector<std::string>& errors,
    std::vector<std::string>& warnings
) {
  if (mvfst.maxCwndInMss == 0) {
    errors.push_back(context + " mvfst: max_cwnd_in_mss must be >= 1");
  }
  if (mvfst.maxConnPacketsSentPerLoop == 0) {
    errors.push_back(context + " mvfst: max_conn_packets_sent_per_loop must be >= 1");
  }
  if (mvfst.numGROBuffers == 0) {
    errors.push_back(context + " mvfst: num_gro_buffers must be >= 1");
  }
  if (mvfst.numGROBuffers > 64) {
    warnings.push_back(
        context + " mvfst: num_gro_buffers (" + std::to_string(mvfst.numGROBuffers) +
        ") exceeds 64; most kernels cap GRO at 64 buffers"
    );
  }
  if (mvfst.copa.deltaParam <= 0.0 || mvfst.copa.deltaParam >= 1.0) {
    errors.push_back(
        context + " mvfst: copa.delta_param (" + std::to_string(mvfst.copa.deltaParam) +
        ") must be in (0, 1)"
    );
  }
  if (mvfst.l4s.ceTarget < 0.0f || mvfst.l4s.ceTarget >= 1.0f) {
    errors.push_back(
        context + " mvfst: l4s.ce_target (" + std::to_string(mvfst.l4s.ceTarget) +
        ") must be 0 (disabled) or in (0, 1)"
    );
  }
}

// For pico listeners, h3zero routes WebTransport CONNECT by prefix-up-to-'?'
// path matching. Only exact service paths can be registered as WT endpoints;
// prefix-path service rules will not route any pico connections.
// Warnings are emitted once, naming all affected pico listeners, since services
// are shared across all listeners.
void validatePicoServicePaths(
    const std::map<std::string, ParsedServiceConfig>& services,
    const std::vector<std::string>& picoListenerNames,
    std::vector<std::string>& warnings
) {
  auto listenerLabel = "Picoquic listener" + std::string(picoListenerNames.size() > 1 ? "s" : "") +
                       " '" + folly::join("', '", picoListenerNames) + "'";

  size_t exactPathCount = 0;
  for (const auto& [svcName, svc] : services) {
    const auto& matchRules = svc.match.value();
    for (size_t j = 0; j < matchRules.size(); ++j) {
      matchRules[j].path.value().visit([&](const auto& alt) {
        using P = std::decay_t<decltype(alt)>;
        if constexpr (std::is_same_v<P, ParsedServiceConfig::MatchRule::PrefixPath>) {
          warnings.push_back(
              listenerLabel + ": service '" + svcName + "' match[" + std::to_string(j) +
              "] has prefix path '" + alt.prefix.value() +
              "' — pico listeners only support exact path routing; "
              "clients using this prefix may fail to connect."
          );
        } else {
          ++exactPathCount;
        }
      });
    }
  }
  if (exactPathCount == 0) {
    warnings.push_back(
        listenerLabel +
        ": no exact-path service match rules found — "
        "no WebTransport endpoints will be registered and all client connections will be rejected."
    );
  }
}

ListenerConfig resolveListener(
    const ParsedListenerConfig& listener,
    const QuicConfig& quic,
    const MvfstConfig& mvfst
) {
  const auto& sock = listener.udp.value().socket.value();
  const auto& tls = listener.tls.value();

  TlsMode tlsMode;
  if (tls.insecure.value()) {
    tlsMode = Insecure{};
  } else {
    tlsMode = resolveTlsConfig(tls);
  }

  const auto& stackStr = listener.quic_stack.value().value_or(kStackMvfst);
  auto quicStack = (stackStr == kStackPicoquic) ? QuicStack::Picoquic : QuicStack::Mvfst;

  return ListenerConfig{
      .name = listener.name.value(),
      .address = folly::SocketAddress(sock.address.value(), sock.port.value()),
      .tlsMode = std::move(tlsMode),
      .endpoint = listener.endpoint.value(),
      .moqtVersions = moqtVersionsToString(listener),
      .quicStack = quicStack,
      .quic = quic,
      .mvfst = mvfst,
  };
}

ServiceConfig::MatchEntry resolveMatchEntry(const ParsedServiceConfig::MatchRule& entry) {
  using ME = ServiceConfig::MatchEntry;
  std::variant<ME::ExactAuthority, ME::WildcardAuthority, ME::AnyAuthority> resolvedAuth;
  entry.authority.value().visit([&](const auto& alt) {
    using A = std::decay_t<decltype(alt)>;
    if constexpr (std::is_same_v<A, ParsedServiceConfig::MatchRule::ExactAuthority>) {
      resolvedAuth = ME::ExactAuthority{alt.exact.value()};
    } else if constexpr (std::is_same_v<A, ParsedServiceConfig::MatchRule::WildcardAuthority>) {
      resolvedAuth = ME::WildcardAuthority{alt.wildcard.value()};
    } else {
      resolvedAuth = ME::AnyAuthority{};
    }
  });

  ME::PathMatcher resolvedPath;
  entry.path.value().visit([&](const auto& alt) {
    using P = std::decay_t<decltype(alt)>;
    if constexpr (std::is_same_v<P, ParsedServiceConfig::MatchRule::ExactPath>) {
      resolvedPath = ME::ExactPath{alt.exact.value()};
    } else {
      resolvedPath = ME::PrefixPath{alt.prefix.value()};
    }
  });

  return ME{
      .authority = std::move(resolvedAuth),
      .path = std::move(resolvedPath),
  };
}

ServiceConfig resolveService(const ParsedServiceConfig& svc, const ParsedCacheConfig& cache) {
  std::vector<ServiceConfig::MatchEntry> entries;
  for (const auto& entry : svc.match.value()) {
    entries.push_back(resolveMatchEntry(entry));
  }
  std::optional<UpstreamConfig> upstream;
  if (svc.upstream.value().has_value()) {
    upstream = resolveUpstream(*svc.upstream.value());
  }
  return ServiceConfig{
      .match = std::move(entries),
      .cache = resolveCacheConfig(cache),
      .upstream = std::move(upstream),
      .auth = resolveAuth(svc.auth.value()),
  };
}

} // namespace

folly::Expected<ResolvedConfig, std::string> resolveConfig(const ParsedConfig& config) {
  std::vector<std::string> errors;
  std::vector<std::string> warnings;

  // === Validate listeners ===

  if (config.listeners.value().empty()) {
    return folly::makeUnexpected(std::string("At least one listener is required"));
  }

  // Extract listener_defaults for use in per-listener merge
  std::optional<ParsedQuicConfig> quicDefaults;
  std::optional<ParsedMvfstConfig> mvfstDefaults;
  if (auto ld = config.listener_defaults.value()) {
    quicDefaults = ld->quic.value();
    mvfstDefaults = ld->mvfst.value();
  }

  std::vector<QuicConfig> mergedQuicConfigs;
  std::vector<MvfstConfig> mergedMvfstConfigs;
  {
    std::unordered_set<std::string> listenerAddrs;
    for (const auto& listener : config.listeners.value()) {
      validateListener(listener, errors, warnings);
      auto addr = listener.udp.value().socket.value().address.value() + ":" +
                  std::to_string(listener.udp.value().socket.value().port.value());
      if (!listenerAddrs.insert(addr).second) {
        errors.push_back("Duplicate listener address: " + addr);
      }
      auto quic = mergeQuicConfig(quicDefaults, listener.quic.value());
      const auto stackStr = listener.quic_stack.value().value_or(kStackMvfst);
      const auto stack = (stackStr == kStackPicoquic) ? QuicStack::Picoquic : QuicStack::Mvfst;
      validateQuicConfig(quic, stack, "Listener '" + listener.name.value() + "'", errors, warnings);
      mergedQuicConfigs.push_back(quic);
      mergedMvfstConfigs.push_back(mergeMvfstConfig(mvfstDefaults, listener.mvfst.value()));
      validateMvfstConfig(
          mergedMvfstConfigs.back(),
          "Listener '" + listener.name.value() + "'",
          errors,
          warnings
      );
      if (!mergedMvfstConfigs.back().pacingEnabled) {
        const auto& cc = quic.ccAlgo;
        if (cc == "bbr" || cc == "bbr2" || cc == "bbr2modular") {
          errors.push_back(
              "Listener '" + listener.name.value() +
              "': mvfst: pacing_enabled: false is incompatible with cc_algo '" + cc +
              "' (bbr, bbr2, and bbr2modular require pacing)"
          );
        }
      }
    }
  }

  // === Validate pico listeners against service paths ===
  // Services are shared, so warnings are emitted once naming all pico listeners.
  {
    std::vector<std::string> picoListenerNames;
    for (const auto& listener : config.listeners.value()) {
      if (listener.quic_stack.value().value_or(kStackMvfst) == kStackPicoquic) {
        picoListenerNames.push_back(listener.name.value());
      }
    }
    if (!picoListenerNames.empty()) {
      validatePicoServicePaths(config.services.value(), picoListenerNames, warnings);
    }
  }

  // === Validate admin ===
  validateAdmin(config, errors);

  // === Validate services ===
  // Per-service upstream config is validated inside validateService().

  const auto& services = config.services.value();
  if (services.empty()) {
    errors.push_back("At least one service is required");
  }

  std::unordered_set<std::string> compositeKeys;
  std::unordered_map<std::string, ParsedCacheConfig> mergedCaches;

  for (const auto& [name, svc] : services) {
    validateService(name, svc, config, compositeKeys, mergedCaches, errors);
  }

  // === Validate threads ===
  const uint32_t threads = config.threads.value().value_or(1);
  if (threads == 0) {
    errors.push_back("threads must be >= 1");
  }

  const bool useRelayThread = config.use_relay_thread.value().value_or(true);
  if (threads > 1 && !useRelayThread) {
    errors.push_back("use_relay_thread must be true when threads > 1");
  }

  const bool mvfstBpfSteering = config.mvfst_bpf_steering.value().value_or(true);

  if (!errors.empty()) {
    return folly::makeUnexpected("Config validation failed:\n  - " + folly::join("\n  - ", errors));
  }

  // === Resolve ===

  folly::F14FastMap<std::string, ServiceConfig> resolvedServices;
  for (const auto& [name, svc] : services) {
    resolvedServices.emplace(name, resolveService(svc, mergedCaches.at(name)));
  }

  // Resolve admin config
  const auto& adminOptional = config.admin.value();
  std::optional<AdminConfig> adminConfig;
  if (adminOptional.has_value()) {
    static const std::vector<std::string> kDefaultAdminAlpn = {"h2", "http/1.1"};
    std::optional<TlsConfig> adminTls;
    if (adminOptional->tls.value().has_value()) {
      adminTls = resolveAdminTlsConfig(*adminOptional->tls.value(), kDefaultAdminAlpn);
    }
    adminConfig = AdminConfig{
        .address =
            folly::SocketAddress(adminOptional->address.value(), adminOptional->port.value()),
        .tls = std::move(adminTls),
    };
  }

  // Resolve relayID: use configured value or generate a random hex string
  std::string relayID = config.relay_id.value().value_or(generateRelayID());

  return ResolvedConfig{
      .config =
          Config{
              .listeners =
                  [&] {
                    std::vector<ListenerConfig> v;
                    v.reserve(config.listeners.value().size());
                    const auto& listeners = config.listeners.value();
                    for (size_t i = 0; i < listeners.size(); ++i) {
                      v.push_back(
                          resolveListener(listeners[i], mergedQuicConfigs[i], mergedMvfstConfigs[i])
                      );
                    }
                    return v;
                  }(),
              .services = std::move(resolvedServices),
              .admin = std::move(adminConfig),
              .relayID = std::move(relayID),
              .threads = threads,
              .useRelayThread = useRelayThread,
              .mvfstBpfSteering = mvfstBpfSteering,
          },
      .warnings = std::move(warnings),
  };
}

} // namespace openmoq::moqx::config
