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
      "mvfst: bbr, bbr2, bbr2modular, copa, cubic, newreno.",
      std::optional<std::string>>
      cc_algo;
};

// rfl::ExtraFields absorbs the "type" discriminator key that the TaggedUnion
// parser has already consumed but that would otherwise be rejected by the
// NoExtraFields processor in strict mode.
struct ParsedTlsInsecure {
  using Tag = rfl::Literal<"insecure">;
  rfl::ExtraFields<rfl::Generic> extra_;
};

struct ParsedTlsFile {
  using Tag = rfl::Literal<"file">;
  rfl::Description<"Path to TLS certificate file (PEM)", std::string> cert_file;
  rfl::Description<"Path to TLS private key file (PEM)", std::string> key_file;
  rfl::ExtraFields<rfl::Generic> extra_;
};

struct ParsedTlsDirectory {
  using Tag = rfl::Literal<"directory">;
  rfl::Description<"Directory containing cert/key pairs (<name>.crt + <name>.key)", std::string>
      cert_dir;
  rfl::Description<
      "SNI identity of the default certificate (optional, first cert if omitted)",
      std::optional<std::string>>
      default_cert;
  rfl::ExtraFields<rfl::Generic> extra_;
};

using ParsedTlsMode =
    rfl::TaggedUnion<"type", ParsedTlsInsecure, ParsedTlsFile, ParsedTlsDirectory>;

struct ParsedListenerConfig {
  rfl::Description<"Listener name", std::string> name;
  rfl::Description<"UDP/QUIC transport config", ParsedUdpConfig> udp;
  ParsedTlsMode tls;
  rfl::Description<"WebTransport endpoint path", std::string> endpoint;
  rfl::Description<
      "MOQT draft versions (empty = all supported)",
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
};

struct ParsedCacheConfig {
  rfl::Description<"Enable relay cache", std::optional<bool>> enabled;
  rfl::Description<"Max cached tracks, ignored when disabled", std::optional<uint32_t>> max_tracks;
  rfl::Description<"Max cached groups per track, ignored when disabled", std::optional<uint32_t>>
      max_groups_per_track;
};

struct ParsedAdminTlsConfig {
  rfl::Description<"Path to TLS certificate file", std::optional<std::string>> cert_file;
  rfl::Description<"Path to TLS private key file", std::optional<std::string>> key_file;
  rfl::Description<"ALPN protocol list", std::optional<std::vector<std::string>>> alpn;
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
};

struct ParsedListenerDefaultsConfig {
  rfl::Description<
      "Default QUIC transport settings for all listeners",
      std::optional<ParsedQuicConfig>>
      quic;
};

struct ParsedServiceDefaultsConfig {
  rfl::Description<"Default cache settings for services", std::optional<ParsedCacheConfig>> cache;
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
};

} // namespace openmoq::moqx::config
