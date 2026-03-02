#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <rfl.hpp>
#include <rfl/yaml.hpp>

#include "o_rly/config/string_literal.h"

namespace openmoq::o_rly::config {

struct SocketConfig {
  static constexpr char kDefaultAddress[] = "::";
  static constexpr uint16_t kDefaultPort = 9668;

  rfl::Description<str_concat<"Bind address (default: \"", kDefaultAddress, "\")">(),
                   std::optional<std::string>>
      address;
  rfl::Description<
      str_concat<"Listen port, 1-65535 (default: ", uint_to_str<kDefaultPort>(), ")">(),
      std::optional<uint16_t>>
      port;

  std::string addressOrDefault() const { return address.value().value_or(kDefaultAddress); }
  uint16_t portOrDefault() const { return port.value().value_or(kDefaultPort); }
};

struct UdpConfig {
  rfl::Description<"Socket configuration", std::optional<SocketConfig>> socket;

  SocketConfig socketOrDefault() const { return socket.value().value_or(SocketConfig{}); }
};

struct TlsCredentials {
  static constexpr bool kDefaultInsecure = false;

  rfl::Description<"Path to TLS certificate file", std::optional<std::string>> cert_file;
  rfl::Description<"Path to TLS private key file", std::optional<std::string>> key_file;
  rfl::Description<
      str_concat<"Skip TLS, insecure mode (default: ", bool_to_str<kDefaultInsecure>(), ")">(),
      std::optional<bool>>
      insecure;

  bool insecureOrDefault() const { return insecure.value().value_or(kDefaultInsecure); }
};

struct ListenerConfig {
  static constexpr char kDefaultEndpoint[] = "/moq-relay";

  rfl::Description<"Listener name", std::string> name;
  rfl::Description<"UDP/QUIC transport config", std::optional<UdpConfig>> udp;
  rfl::Description<"TLS credentials", std::optional<TlsCredentials>> tls_credentials;
  rfl::Description<str_concat<"WebTransport endpoint path (default: \"", kDefaultEndpoint, "\")">(),
                   std::optional<std::string>>
      endpoint;
  rfl::Description<"MOQT draft versions (empty = all supported)",
                   std::optional<std::vector<uint32_t>>>
      moqt_versions;

  std::string endpointOrDefault() const { return endpoint.value().value_or(kDefaultEndpoint); }
};

struct CacheConfig {
  static constexpr bool kDefaultEnabled = true;
  static constexpr uint32_t kDefaultMaxTracks = 100;
  static constexpr uint32_t kDefaultMaxGroupsPerTrack = 3;

  rfl::Description<
      str_concat<"Enable relay cache (default: ", bool_to_str<kDefaultEnabled>(), ")">(),
      std::optional<bool>>
      enabled;
  rfl::Description<str_concat<"Max cached tracks, ignored when disabled (default: ",
                              uint_to_str<kDefaultMaxTracks>(), ")">(),
                   std::optional<uint32_t>>
      max_tracks;
  rfl::Description<str_concat<"Max cached groups per track, ignored when disabled (default: ",
                              uint_to_str<kDefaultMaxGroupsPerTrack>(), ")">(),
                   std::optional<uint32_t>>
      max_groups_per_track;

  bool enabledOrDefault() const { return enabled.value().value_or(kDefaultEnabled); }
  uint32_t maxTracksOrDefault() const { return max_tracks.value().value_or(kDefaultMaxTracks); }
  uint32_t maxGroupsPerTrackOrDefault() const {
    return max_groups_per_track.value().value_or(kDefaultMaxGroupsPerTrack);
  }
};

struct Config {
  rfl::Description<"Listener definitions (currently exactly one supported)",
                   std::vector<ListenerConfig>>
      listeners;
  rfl::Description<str_concat<"Relay cache settings (default: ",
                              bool_to_str<CacheConfig::kDefaultEnabled>(), ")">(),
                   std::optional<CacheConfig>>
      cache;

  CacheConfig cacheOrDefault() const { return cache.value().value_or(CacheConfig{}); }
};

} // namespace openmoq::o_rly::config
