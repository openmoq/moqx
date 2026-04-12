/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

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
  uint32_t maxCachedMb{16};   // code default: 16 MB; 0 invalid
  uint32_t minEvictionKb{64}; // eviction batch floor in KB
  std::chrono::milliseconds maxCacheDuration{std::chrono::hours(24)}; // cap on any track duration
  std::optional<std::chrono::milliseconds>
      defaultMaxCacheDuration; // nullopt = use maxCacheDuration; 0ms = opt-in only
};

enum class QuicStack { Mvfst, Picoquic };

struct QuicConfig {
  // Flow control
  uint64_t maxData{67108864};       // connection flow control window (bytes)
  uint64_t maxStreamData{16777216}; // per-stream flow control window (bytes)
  uint64_t maxUniStreams{8192};     // max concurrent unidirectional streams
  uint64_t maxBidiStreams{16};      // max concurrent bidirectional streams

  // Transport settings
  uint64_t idleTimeoutMs{30000};      // idle timeout (ms)
  uint32_t maxAckDelayUs{25000};      // max ACK delay (us); mvfst ignores (logs DBG1)
  uint32_t minAckDelayUs{1000};       // min ACK delay (microseconds)
  uint8_t defaultStreamPriority{2};   // default stream priority
  uint8_t defaultDatagramPriority{1}; // default datagram priority
  std::string ccAlgo{"copa"};         // congestion control algorithm name
};

struct ListenerConfig {
  std::string name;
  folly::SocketAddress address;
  TlsMode tlsMode;
  std::string endpoint;
  std::string moqtVersions; // comma-separated string
  QuicStack quicStack{QuicStack::Mvfst};
  QuicConfig quic; // merged from listener_defaults.quic + per-listener quic override
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
