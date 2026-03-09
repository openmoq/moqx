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
};

struct ParsedCacheConfig {
  rfl::Description<"Enable relay cache", bool> enabled;
  rfl::Description<"Max cached tracks, ignored when disabled", uint32_t> max_tracks;
  rfl::Description<"Max cached groups per track, ignored when disabled", uint32_t>
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

struct ParsedConfig {
  rfl::Description<
      "Listener definitions (currently exactly one supported)",
      std::vector<ParsedListenerConfig>>
      listeners;
  rfl::Description<"Relay cache settings", ParsedCacheConfig> cache;
  rfl::Description<"Admin HTTP server settings", std::optional<ParsedAdminConfig>> admin;
};

} // namespace openmoq::o_rly::config
