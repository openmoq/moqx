#pragma once

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
};

struct AdminConfig {
  folly::SocketAddress address;
  std::optional<TlsConfig> tls;
};

struct Config {
  ListenerConfig listener;
  folly::F14FastMap<std::string, ServiceConfig> services;
  std::optional<AdminConfig> admin;
};

} // namespace openmoq::moqx::config
