#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <folly/SocketAddress.h>

namespace openmoq::o_rly::tls {
class TlsCertProvider;
} // namespace openmoq::o_rly::tls

namespace openmoq::o_rly::config {

struct TlsConfig {
  std::string certFile;
  std::string keyFile;
  std::vector<std::string> alpn; // must be empty for QUIC listener: ALPN derived from moqt_versions
};

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

struct AdminConfig {
  folly::SocketAddress address;
  std::optional<TlsConfig> tls;
};

struct Config {
  ListenerConfig listener;
  CacheConfig cache;
  std::optional<AdminConfig> admin;
};

} // namespace openmoq::o_rly::config
