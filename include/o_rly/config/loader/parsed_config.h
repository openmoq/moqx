#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <rfl.hpp>
#include <rfl/yaml.hpp>

namespace openmoq::o_rly::config {

// Required config field
#define ORLY_CONFIG(desc, Type, name) rfl::Description<desc, Type> name

// Optional config field
#define ORLY_CONFIG_OPT(desc, Type, name) rfl::Description<desc, std::optional<Type>> name

struct ParsedSocketConfig {
  ORLY_CONFIG("Bind address", std::string, address);
  ORLY_CONFIG("Listen port, 1-65535", uint16_t, port);
};

struct ParsedUdpConfig {
  ORLY_CONFIG("Socket configuration", ParsedSocketConfig, socket);
};

struct ParsedTlsConfig {
  ORLY_CONFIG_OPT("Path to TLS certificate file", std::string, cert_file);
  ORLY_CONFIG_OPT("Path to TLS private key file", std::string, key_file);
  ORLY_CONFIG("Insecure mode, use default compiled-in cert", bool, insecure);
};

struct ParsedListenerConfig {
  ORLY_CONFIG("Listener name", std::string, name);
  ORLY_CONFIG("UDP/QUIC transport config", ParsedUdpConfig, udp);
  ORLY_CONFIG("TLS configuration", ParsedTlsConfig, tls);
  ORLY_CONFIG("WebTransport endpoint path", std::string, endpoint);
  ORLY_CONFIG_OPT("MOQT draft versions (empty = all supported)", std::vector<uint32_t>, moqt_versions);
};

struct ParsedCacheConfig {
  ORLY_CONFIG("Enable relay cache", bool, enabled);
  ORLY_CONFIG("Max cached tracks, ignored when disabled", uint32_t, max_tracks);
  ORLY_CONFIG("Max cached groups per track, ignored when disabled", uint32_t, max_groups_per_track);
};

struct ParsedConfig {
  ORLY_CONFIG("Listener definitions (currently exactly one supported)", std::vector<ParsedListenerConfig>, listeners);
  ORLY_CONFIG("Relay cache settings", ParsedCacheConfig, cache);
};

} // namespace openmoq::o_rly::config
