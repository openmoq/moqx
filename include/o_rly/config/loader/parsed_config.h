#pragma once

#include <cstdint>
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

struct ParsedTlsConfig {
  rfl::Description<"Path to TLS certificate file", std::optional<std::string>> cert_file;
  rfl::Description<"Path to TLS private key file", std::optional<std::string>> key_file;
  rfl::Description<"Insecure mode, use default compiled-in cert", bool> insecure;
};

struct ParsedListenerConfig {
  rfl::Description<"Listener name", std::string> name;
  rfl::Description<"UDP/QUIC transport config", ParsedUdpConfig> udp;
  rfl::Description<"TLS configuration", ParsedTlsConfig> tls;
  rfl::Description<"WebTransport endpoint path", std::string> endpoint;
  rfl::Description<
      "MOQT draft versions (empty = all supported)",
      std::optional<std::vector<uint32_t>>>
      moqt_versions;
};

struct ParsedCacheConfig {
  rfl::Description<"Enable relay cache", bool> enabled;
  rfl::Description<"Max cached tracks, ignored when disabled", uint32_t> max_tracks;
  rfl::Description<"Max cached groups per track, ignored when disabled", uint32_t>
      max_groups_per_track;
};

struct ParsedConfig {
  rfl::Description<
      "Listener definitions (currently exactly one supported)",
      std::vector<ParsedListenerConfig>>
      listeners;
  rfl::Description<"Relay cache settings", ParsedCacheConfig> cache;
};

} // namespace openmoq::o_rly::config
