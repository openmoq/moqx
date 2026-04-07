#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <folly/SocketAddress.h>
#include <folly/container/F14Map.h>

namespace openmoq::moqx::config {

struct TlsConfig {
  std::string certFile;
  std::string keyFile;
  std::vector<std::string> alpn; // must be empty for QUIC listener: ALPN derived from moqt_versions
};

struct Insecure {};

using TlsMode = std::variant<Insecure, TlsConfig>;

struct CacheConfig {
  size_t maxCachedTracks; // 0 when cache disabled
  size_t maxCachedGroupsPerTrack;
};

struct ListenerConfig {
  std::string name;
  folly::SocketAddress address;
  TlsMode tlsMode;
  std::string endpoint;
  std::string moqtVersions; // comma-separated string
};

struct UpstreamTlsConfig {
  bool insecure{false};
  std::optional<std::string> caCertFile; // mutually exclusive with insecure=true
};

struct UpstreamConfig {
  std::string url;
  UpstreamTlsConfig tls;
  std::chrono::milliseconds connectTimeout{5000};
  std::chrono::milliseconds idleTimeout{5000};
};

struct ServiceConfig {
  struct MatchEntry {
    struct ExactAuthority {
      std::string value;
    };
    struct WildcardAuthority {
      std::string pattern; // e.g. "*.example.com"
    };
    struct AnyAuthority {};

    struct ExactPath {
      std::string value;
    };
    struct PrefixPath {
      std::string value;
    };
    using PathMatcher = std::variant<ExactPath, PrefixPath>;

    std::variant<ExactAuthority, WildcardAuthority, AnyAuthority> authority;
    PathMatcher path; // PrefixPath{"/"} matches any path
  };

  std::vector<MatchEntry> match;
  CacheConfig cache;
  std::optional<UpstreamConfig> upstream; // set if this service chains to an upstream relay
};

struct AdminConfig {
  folly::SocketAddress address;
  std::optional<TlsConfig> tls;
};

struct Config {
  std::vector<ListenerConfig> listeners;
  folly::F14FastMap<std::string, ServiceConfig> services;
  std::optional<AdminConfig> admin;
  std::string relayID; // always set: from config or randomly generated
  uint32_t threads{1};
};

} // namespace openmoq::moqx::config
