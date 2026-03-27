#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <rfl.hpp>
#include <rfl/yaml.hpp>

namespace openmoq::o_rly::config {

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
  rfl::Description<"Insecure mode, use default compiled-in cert", bool> insecure;
};

struct ParsedAdminTlsConfig {
  rfl::Description<"Path to TLS certificate file", std::optional<std::string>> cert_file;
  rfl::Description<"Path to TLS private key file", std::optional<std::string>> key_file;
  rfl::Description<"ALPN protocol list", std::optional<std::vector<std::string>>> alpn;
};

struct ParsedListenerConfig {
  rfl::Description<"Listener name", std::string> name;
  rfl::Description<"UDP/QUIC transport config", ParsedUdpConfig> udp;
  rfl::Description<"TLS configuration", ParsedListenerTlsConfig> tls;
  rfl::Description<"WebTransport endpoint path", std::string> endpoint;
  rfl::Description<
      "MOQT draft versions (empty = all supported)",
      std::optional<std::vector<uint32_t>>>
      moqt_versions;
};

struct ParsedCacheConfig {
  rfl::Description<"Enable relay cache", std::optional<bool>> enabled;
  rfl::Description<"Max cached tracks, ignored when disabled", std::optional<uint32_t>> max_tracks;
  rfl::Description<"Max cached groups per track, ignored when disabled", std::optional<uint32_t>>
      max_groups_per_track;
};

struct ParsedAdminConfig {
  rfl::Description<"HTTP admin server port, 1-65535", uint16_t> port;
  rfl::Description<"Bind address", std::string> address;
  rfl::Description<"Allow plain HTTP (mutually exclusive with tls)", bool> plaintext;
  rfl::Description<"TLS configuration", std::optional<ParsedAdminTlsConfig>> tls;
};

struct ParsedUpstreamTlsConfig {
  rfl::Description<"Skip TLS certificate verification (dev only)", bool> insecure;
  rfl::Description<"Path to CA certificate file for peer verification",
                   std::optional<std::string>> ca_cert;
};

struct ParsedUpstreamConfig {
  rfl::Description<"Upstream MoQ server URL (moqt://host:port/path)", std::string> url;
  rfl::Description<"TLS configuration for upstream connection", ParsedUpstreamTlsConfig> tls;
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
  rfl::Description<"Upstream MoQ server for this service (optional; enables relay chaining)",
                   std::optional<ParsedUpstreamConfig>> upstream;
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
  rfl::Description<"Relay identity string (optional; random string generated if absent)",
                   std::optional<std::string>> relay_id;
};

} // namespace openmoq::o_rly::config
