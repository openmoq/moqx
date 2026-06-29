/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <rfl.hpp>
#include <rfl/yaml.hpp>

namespace openmoq::moqx::config {

// Note: fields wrapped in rfl::Description<"...", T> expose a .value() accessor
// that unwraps the Description wrapper — this is NOT std::optional::value().
// For optional fields the type is rfl::Description<"...", std::optional<T>>,
// so .value() returns the std::optional, and a second .value() or
// .value_or() unwraps the optional itself.

struct ParsedSocketConfig {
  rfl::Description<"Bind address", std::string> address;
  rfl::Description<"Listen port, 1-65535", uint16_t> port;
};

struct ParsedUdpConfig {
  rfl::Description<"Socket configuration", ParsedSocketConfig> socket;
};

struct ParsedListenerTlsConfig {
  rfl::Description<"Path to TLS certificate file", std::optional<std::string>> cert_file;
  rfl::Description<"Path to TLS private key file", std::optional<std::string>> key_file;
  rfl::Description<
      "Path to a PKCS#12 bundle (.p12/.pfx) carrying the cert chain + private key; "
      "mutually exclusive with cert_file/key_file. Decrypted in memory (key never hits disk).",
      std::optional<std::string>>
      pkcs12_file;
  rfl::Description<
      "Inline PKCS#12 password (discouraged; prefer pkcs12_password_file). Empty for "
      "password-less bundles.",
      std::optional<std::string>>
      pkcs12_password;
  rfl::Description<
      "Path to a file containing the PKCS#12 password (preferred over inline).",
      std::optional<std::string>>
      pkcs12_password_file;
  rfl::Description<"Insecure mode, use default compiled-in cert", bool> insecure;
};

struct ParsedAdminTlsConfig {
  rfl::Description<"Path to TLS certificate file", std::optional<std::string>> cert_file;
  rfl::Description<"Path to TLS private key file", std::optional<std::string>> key_file;
  rfl::Description<
      "Path to a PKCS#12 bundle (.p12/.pfx) carrying the cert chain + private key; "
      "mutually exclusive with cert_file/key_file. Decrypted in memory (key never hits disk).",
      std::optional<std::string>>
      pkcs12_file;
  rfl::Description<
      "Inline PKCS#12 password (discouraged; prefer pkcs12_password_file). Empty for "
      "password-less bundles.",
      std::optional<std::string>>
      pkcs12_password;
  rfl::Description<
      "Path to a file containing the PKCS#12 password (preferred over inline).",
      std::optional<std::string>>
      pkcs12_password_file;
  rfl::Description<"ALPN protocol list", std::optional<std::vector<std::string>>> alpn;
};

struct ParsedQuicConfig {
  // Flow control
  rfl::Description<
      "Connection flow control window in bytes (default: 64MB)",
      std::optional<uint64_t>>
      max_data;
  rfl::Description<
      "Per-stream flow control window in bytes (default: 16MB)",
      std::optional<uint64_t>>
      max_stream_data;
  rfl::Description<"Max concurrent unidirectional streams (default: 8192)", std::optional<uint64_t>>
      max_uni_streams;
  rfl::Description<"Max concurrent bidirectional streams (default: 16)", std::optional<uint64_t>>
      max_bidi_streams;

  // Transport settings
  rfl::Description<"Idle timeout in milliseconds (default: 30000)", std::optional<uint64_t>>
      idle_timeout_ms;
  rfl::Description<
      "Max ACK delay in microseconds (default: 25000, QUIC spec default); "
      "mvfst ignores this field (logs DBG1 if set)",
      std::optional<uint32_t>>
      max_ack_delay_us;
  rfl::Description<"Min ACK delay in microseconds (default: 1000)", std::optional<uint32_t>>
      min_ack_delay_us;
  rfl::Description<"Default stream priority (default: 2)", std::optional<uint8_t>>
      default_stream_priority;
  rfl::Description<"Default datagram priority (default: 1)", std::optional<uint8_t>>
      default_datagram_priority;
  rfl::Description<
      "Congestion control algorithm (default: bbr). "
      "picoquic: bbr, bbr1, c4, cubic, dcubic, fast, newreno, prague, reno. "
      "mvfst: bbr, bbr2, bbr2modular, copa, cubic, newreno, none.",
      std::optional<std::string>>
      cc_algo;
};

// mvfst-specific transport tunables organised by CC algorithm.
// General fields (max_cwnd_in_mss, GSO/GRO) sit directly at the mvfst: level.
// CC-specific tunables are nested under mvfst: <algo>: blocks.
// All fields are optional; absent = use MvfstConfig defaults.
struct ParsedMvfstConfig {
  rfl::Description<
      "Maximum congestion window in MSS (default: 860000 = quic::kLargeMaxCwndInMss). "
      "Applies to all CC algorithms.",
      std::optional<uint64_t>>
      max_cwnd_in_mss;
  rfl::Description<
      "Enable pacing (default: true). "
      "Required for bbr and bbr2; optional for copa and cubic; "
      "ignored (always off) for newreno and none.",
      std::optional<bool>>
      pacing_enabled;
  rfl::Description<
      "Enable GSO (Generic Segmentation Offload) for packet segmentation (default: true). "
      "Requires kernel GSO support. Falls back to sendmmsg automatically if GSO is "
      "unavailable at runtime. Disable only on kernels or VMs where GSO is advertised "
      "but broken.",
      std::optional<bool>>
      enable_gso;
  rfl::Description<
      "Max packets written per connection per event-loop iteration (default: 48). "
      "Written to both maxBatchSize and writeConnectionDataPacketsLimit so the cap "
      "applies in all batching modes.",
      std::optional<uint32_t>>
      max_conn_packets_sent_per_loop;
  rfl::Description<
      "Use recvmmsg for batch receives in QuicServerWorker (default: true). "
      "Enables the recvmmsg kernel call to read multiple UDP packets per syscall, "
      "reducing overhead at high packet rates.",
      std::optional<bool>>
      use_recvmmsg;
  rfl::Description<
      "Max incoming packets processed per event-loop read callback. "
      "0 = unlimited. Default 64.",
      std::optional<uint16_t>>
      max_server_recv_packets_per_loop;
  rfl::Description<
      "Number of UDP GRO (Generic Receive Offload) buffers. "
      "1 = disabled (default). Values > 1 enable kernel-side packet coalescing: "
      "up to max value packets are merged per recvmsg call, reducing syscall "
      "overhead at high packet rates. Max 64. Requires kernel GRO support.",
      std::optional<uint32_t>>
      num_gro_buffers;
  rfl::Description<
      "Ignore the peer's path MTU and send full-size (max UDP payload) packets "
      "(default: false). Useful for testing or controlled networks where the "
      "path MTU is known; leave off to respect PMTU discovery.",
      std::optional<bool>>
      ignore_path_mtu;

  // BBR (BBR1) congestion control tunables.
  // conservative_recovery is shared with BBR2 — set it here when using either.
  struct ParsedBBR {
    rfl::Description<
        "Enter conservative recovery on loss (also applies to BBR2).",
        std::optional<bool>>
        conservative_recovery;
    rfl::Description<"Use a large cwnd during PROBE_RTT.", std::optional<bool>>
        large_probe_rtt_cwnd;
    rfl::Description<
        "Estimate ACK aggregation in STARTUP for more aggressive cwnd growth.",
        std::optional<bool>>
        enable_ack_aggregation_in_startup;
    rfl::Description<"Skip PROBE_RTT rounds when the sender is app-limited.", std::optional<bool>>
        probe_rtt_disabled_if_app_limited;
    rfl::Description<"Drain cwnd to the estimated BDP after STARTUP.", std::optional<bool>>
        drain_to_target;
  };

  // BBR2-specific congestion control tunables.
  // Also set bbr.conservative_recovery if you want conservative recovery with BBR2.
  struct ParsedBBR2 {
    rfl::Description<
        "Ignore long-term inflight samples when estimating bandwidth.",
        std::optional<bool>>
        ignore_inflight_long_term;
    rfl::Description<"Ignore short-term bandwidth samples.", std::optional<bool>> ignore_short_term;
    rfl::Description<"Exit STARTUP on packet loss (default: true).", std::optional<bool>>
        exit_startup_on_loss;
    rfl::Description<"Allow cwnd recovery inside STARTUP (default: true).", std::optional<bool>>
        enable_recovery_in_startup;
    rfl::Description<
        "Allow cwnd recovery during PROBE_BW and PROBE_RTT states (default: true).",
        std::optional<bool>>
        enable_recovery_in_probe_states;
    rfl::Description<"Coexist with Reno/Cubic flows (halve cwnd on loss).", std::optional<bool>>
        enable_reno_coexistence;
    rfl::Description<"Pace the initial cwnd instead of sending it as a burst.", std::optional<bool>>
        pace_init_cwnd;
    rfl::Description<
        "Override the PROBE_BW cruise pacing gain; -1 = use BBR2 default (~1.0).",
        std::optional<float>>
        override_cruise_pacing_gain;
    rfl::Description<
        "Override the PROBE_BW cruise cwnd gain; -1 = use BBR2 default (~2.0).",
        std::optional<float>>
        override_cruise_cwnd_gain;
    rfl::Description<
        "Override the STARTUP pacing gain; -1 = use BBR2 default (~2.77).",
        std::optional<float>>
        override_startup_pacing_gain;
    rfl::Description<
        "Beta used to reduce the short-term bandwidth estimate; 0 = use BBR2 default.",
        std::optional<float>>
        override_bw_short_beta;
  };

  // Cubic congestion control tunables.
  struct ParsedCubic {
    rfl::Description<
        "Use additive cwnd increase instead of multiplicative after HyStart exit.",
        std::optional<bool>>
        additive_increase_after_hystart;
    rfl::Description<"Only grow cwnd when the connection is cwnd-limited.", std::optional<bool>>
        only_grow_cwnd_when_limited;
    rfl::Description<
        "Leave headroom below cwnd for the OS send buffer when cwnd-limited.",
        std::optional<bool>>
        leave_headroom_for_cwnd_limited;
  };

  // Copa congestion control tunables.
  struct ParsedCopa {
    rfl::Description<
        "Delta parameter controlling the target utilisation (moqx default: 0.05).",
        std::optional<double>>
        delta_param;
  };

  // L4S tunables (usable alongside any ECN-aware CC algorithm).
  struct ParsedL4S {
    rfl::Description<"ECN CE marking threshold target (0 = disabled).", std::optional<float>>
        ce_target;
  };

  rfl::Description<
      "BBR congestion control tunables. conservative_recovery also applies to BBR2.",
      std::optional<ParsedBBR>>
      bbr;
  rfl::Description<"BBR2-specific congestion control tunables.", std::optional<ParsedBBR2>> bbr2;
  rfl::Description<"Cubic congestion control tunables.", std::optional<ParsedCubic>> cubic;
  rfl::Description<"Copa congestion control tunables.", std::optional<ParsedCopa>> copa;
  rfl::Description<
      "L4S tunables (ECN CE target). Usable alongside any ECN-aware CC algorithm.",
      std::optional<ParsedL4S>>
      l4s;
  rfl::Description<
      "UDP socket send and receive buffer size in bytes. Default: 0 (use MoQServer built-in "
      "default of 1 MB). Increase to reduce EAGAIN errors under high fan-out burst load.",
      std::optional<uint64_t>>
      udp_socket_buffer_bytes;
};

struct ParsedListenerConfig {
  rfl::Description<"Listener name", std::string> name;
  rfl::Description<"UDP/QUIC transport config", ParsedUdpConfig> udp;
  rfl::Description<"TLS configuration", ParsedListenerTlsConfig> tls;
  rfl::Description<"WebTransport endpoint path", std::string> endpoint;
  rfl::Description<
      "MOQT draft versions (empty = default 14,16)",
      std::optional<std::vector<uint32_t>>>
      moqt_versions;
  rfl::Description<
      "QUIC stack to use: \"mvfst\" (default) or \"picoquic\"",
      std::optional<std::string>>
      quic_stack;
  rfl::Description<
      "QUIC transport settings (overrides listener_defaults.quic)",
      std::optional<ParsedQuicConfig>>
      quic;
  rfl::Description<
      "mvfst-specific CC tunables (overrides listener_defaults.mvfst; mvfst stack only)",
      std::optional<ParsedMvfstConfig>>
      mvfst;
};

struct ParsedCacheConfig {
  rfl::Description<"Enable relay cache", std::optional<bool>> enabled;
  rfl::Description<"Max cached tracks, ignored when disabled", std::optional<uint32_t>> max_tracks;
  rfl::Description<"Max cached groups per track, ignored when disabled", std::optional<uint32_t>>
      max_groups_per_track;
  rfl::Description<
      "Max total cache size in megabytes across all tracks. Default: 16. 0 is invalid. "
      "Ignored when cache is disabled.",
      std::optional<uint32_t>>
      max_cached_mb;
  rfl::Description<
      "Eviction batch floor in kilobytes: when the cache exceeds the byte limit, "
      "evict LRU tracks until usage falls to max_cached_mb - min_eviction_kb. "
      "Default: 64. Ignored when cache is disabled.",
      std::optional<uint32_t>>
      min_eviction_kb;
  rfl::Description<
      "Maximum cache duration (seconds) for any track; clamps publisher-set values. "
      "Also used as the default for tracks without a publisher-set duration when "
      "default_max_cache_duration_s is absent. Default: 86400 (1 day). 0 is invalid. "
      "Ignored when cache is disabled.",
      std::optional<uint32_t>>
      max_cache_duration_s;
  rfl::Description<
      "Default max cache duration (seconds) for tracks without a publisher-set cache duration. "
      "Absent: use max_cache_duration_s as the default. "
      "0: opt-in-only — do not cache tracks unless the publisher sets a cache duration. "
      "N: use N seconds as the default for tracks without a publisher-set cache duration. "
      "Ignored when cache is disabled.",
      std::optional<uint32_t>>
      default_max_cache_duration_s;
};

struct ParsedAdminConfig {
  rfl::Description<"HTTP admin server port, 1-65535", uint16_t> port;
  rfl::Description<"Bind address", std::string> address;
  rfl::Description<"Allow plain HTTP (mutually exclusive with tls)", bool> plaintext;
  rfl::Description<"TLS configuration", std::optional<ParsedAdminTlsConfig>> tls;
};

struct ParsedUpstreamTlsConfig {
  rfl::Description<"Skip TLS certificate verification (dev only)", bool> insecure;
  rfl::Description<"Path to CA certificate file for peer verification", std::optional<std::string>>
      ca_cert;
};

struct ParsedUpstreamConfig {
  rfl::Description<"Upstream MoQ server URL (moqt://host:port/path)", std::string> url;
  rfl::Description<"TLS configuration for upstream connection", ParsedUpstreamTlsConfig> tls;
  rfl::Description<"QUIC connect timeout in milliseconds (default: 5000)", std::optional<uint32_t>>
      connect_timeout_ms;
  rfl::Description<
      "MoQ session idle timeout in milliseconds (default: 5000)",
      std::optional<uint32_t>>
      idle_timeout_ms;
};

struct ParsedAuthConfig {
  struct HmacKey {
    rfl::Description<"Key identifier embedded in the token envelope", std::string> id;
    rfl::Description<"Shared HMAC secret for this key id", std::string> secret;
  };

  rfl::Description<"Enable per-service authorization", bool> enabled;
  rfl::Description<"Expected MOQT AUTHORIZATION_TOKEN token type", std::optional<uint64_t>>
      token_type;
  rfl::Description<"Accepted HMAC signing keys", std::optional<std::vector<HmacKey>>> hmac_keys;
  rfl::Description<
      "Require a valid setup token during CLIENT_SETUP (default: true)",
      std::optional<bool>>
      require_setup_token;
  rfl::Description<
      "Allow request AUTHORIZATION_TOKEN to override setup credentials (default: true)",
      std::optional<bool>>
      allow_request_token_override;
  rfl::Description<
      "Reject unsupported token claims instead of ignoring them (default: false)",
      std::optional<bool>>
      strict_claims;
};

struct ParsedServiceConfig {
  struct MatchRule {
    struct ExactAuthority {
      rfl::Description<"Exact authority to match", std::string> exact;
    };
    struct WildcardAuthority {
      rfl::Description<
          "Wildcard pattern, e.g. '*.example.com'. "
          "Matches single-label subdomains only (foo.example.com), "
          "not the bare domain (example.com) or multi-label (a.b.example.com).",
          std::string>
          wildcard;
    };
    struct AnyAuthority {
      rfl::Description<"Must be true", bool> any;
    };
    using AuthorityMatch = rfl::Variant<ExactAuthority, WildcardAuthority, AnyAuthority>;

    struct ExactPath {
      rfl::Description<"Exact path to match, must start with '/'", std::string> exact;
    };
    struct PrefixPath {
      rfl::Description<
          "Path prefix to match, must start with '/'. "
          "Uses simple string prefix matching (not segment-aware): "
          "prefix '/abc' matches '/abc', '/abc/def', and '/abcdef'. "
          "Use a trailing '/' (e.g. '/abc/') to restrict to path segments.",
          std::string>
          prefix;
    };
    using PathMatch = rfl::Variant<ExactPath, PrefixPath>;

    rfl::Description<"Authority matcher", AuthorityMatch> authority;
    rfl::Description<"Path matcher", PathMatch> path;
  };

  rfl::Description<"Match rules for routing", std::vector<MatchRule>> match;
  rfl::Description<
      "Per-service cache settings (overrides service_defaults)",
      std::optional<ParsedCacheConfig>>
      cache;
  rfl::Description<
      "Upstream MoQ server for this service (optional; enables relay chaining)",
      std::optional<ParsedUpstreamConfig>>
      upstream;
  rfl::Description<
      "Authentication and authorization settings for this service",
      std::optional<ParsedAuthConfig>>
      auth;
};

struct ParsedListenerDefaultsConfig {
  rfl::Description<
      "Default QUIC transport settings for all listeners",
      std::optional<ParsedQuicConfig>>
      quic;
  rfl::Description<"Default mvfst CC tunables for all listeners", std::optional<ParsedMvfstConfig>>
      mvfst;
};

struct ParsedServiceDefaultsConfig {
  rfl::Description<"Default cache settings for services", std::optional<ParsedCacheConfig>> cache;
};

struct ParsedMLogConfig {
  rfl::Description<"Directory for per-session MoQ log files (empty = disabled)", std::string> dir;
  rfl::Description<"Fraction of sessions to log (0.0-1.0, default 0.0)", std::optional<float>>
      sample_rate;
};

struct ParsedQLogConfig {
  rfl::Description<"Directory for per-connection QUIC qlog files (empty = disabled)", std::string>
      dir;
  rfl::Description<
      "Fraction of connections to log (0.0-1.0, default 0.0; mvfst only)",
      std::optional<float>>
      sample_rate;
};

struct ParsedLoggingConfig {
  rfl::Description<"MoQ-level (mlog) per-session logging", std::optional<ParsedMLogConfig>> mlog;
  rfl::Description<"QUIC-level (qlog) per-connection logging", std::optional<ParsedQLogConfig>>
      qlog;
};

struct ParsedConfig {
  rfl::Description<
      "Listener definitions (currently exactly one supported)",
      std::vector<ParsedListenerConfig>>
      listeners;
  rfl::Description<
      "Default settings inherited by all services",
      std::optional<ParsedServiceDefaultsConfig>>
      service_defaults;
  rfl::Description<"Service definitions", std::map<std::string, ParsedServiceConfig>> services;
  rfl::Description<"Admin HTTP server settings", std::optional<ParsedAdminConfig>> admin;
  rfl::Description<
      "Relay identity string (optional; random string generated if absent)",
      std::optional<std::string>>
      relay_id;
  rfl::Description<
      "Default settings inherited by all listeners",
      std::optional<ParsedListenerDefaultsConfig>>
      listener_defaults;
  rfl::Description<"Number of IO worker threads (default: 1)", std::optional<uint32_t>> threads;
  rfl::Description<
      "Dedicate one relay thread per service for relay state isolation (default: true). "
      "Disable for baseline performance comparison.",
      std::optional<bool>>
      use_relay_thread;
  rfl::Description<
      "Use per-subscriber-thread local forwarders to minimize cross-thread hops on the "
      "data path (requires use_relay_thread; default: false). Disable to run all subscribes "
      "on the relay thread via subscribeImpl.",
      std::optional<bool>>
      use_local_forwarders;
  rfl::Description<
      "Attach a classic BPF reuseport filter to steer QUIC packets to the correct mvfst worker "
      "based on the connection ID's workerId field (Linux only, mvfst stack only, default: true). "
      "Disable to fall back to kernel RSS distribution.",
      std::optional<bool>>
      mvfst_bpf_steering;
  rfl::Description<"Logging configuration", std::optional<ParsedLoggingConfig>> logging;
};

} // namespace openmoq::moqx::config
