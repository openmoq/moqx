#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <folly/SocketAddress.h>

namespace openmoq::o_rly::tls {
class TlsCertProvider;
} // namespace openmoq::o_rly::tls

namespace openmoq::o_rly::config {

struct CacheConfig {
  size_t maxCachedTracks; // 0 when cache disabled
  size_t maxCachedGroupsPerTrack;
};

struct ListenerConfig {
  std::string name;
  folly::SocketAddress address;
  std::shared_ptr<tls::TlsCertProvider> tlsProvider;
  std::string endpoint;
  std::string moqtVersions; // comma-separated string
};

struct Config {
  ListenerConfig listener;
  CacheConfig cache;
  uint16_t adminPort;
};

} // namespace openmoq::o_rly::config
