#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>

#include <folly/SocketAddress.h>

namespace openmoq::o_rly::config {

struct TlsConfig {
  std::string certFile;
  std::string keyFile;
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

struct Config {
  ListenerConfig listener;
  CacheConfig cache;
  uint16_t adminPort;
};

} // namespace openmoq::o_rly::config
