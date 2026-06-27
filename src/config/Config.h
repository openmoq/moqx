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

// ProxygenQmux carries MoQ over QMUX-on-TCP + Fizz TLS, not QUIC/UDP.
enum class QuicStack { Mvfst, Picoquic, ProxygenQmux };

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
  std::string ccAlgo{"bbr"};          // congestion control algorithm name
};

// mvfst-specific transport tunables organised by congestion-control algorithm.
// Defaults match mvfst's own defaults except where moqx intentionally overrides
// them (noted inline).  CC-specific fields are in sub-structs that map to
// mvfst: <algo>: blocks in YAML.  General fields sit directly at the mvfst: level.
struct MvfstConfig {
  // Max congestion window in MSS (quic::kLargeMaxCwndInMss = 860000).
  uint64_t maxCwndInMss{860000};

  // Pacing: enabled by default. BBR and BBR2 require pacing; Copa and Cubic
  // support optional pacing; NewReno and None are always unpaced.
  bool pacingEnabled{true};

  // Send-path batching: sendmmsg + gso select the QuicBatchingMode (2×2).
  bool enableGSO{true}; // GSO segmentation; falls back to sendmmsg if unavailable at runtime
  // Per-connection write loop limit. Written to both maxBatchSize and
  // writeConnectionDataPacketsLimit so the cap is honored in all batching modes.
  uint32_t maxConnPacketsSentPerLoop{48};

  // Receive-path tuning.
  // Use recvmmsg for batch receives in QuicServerWorker.
  bool useRecvmmsg{true};
  // Max incoming packets processed per event-loop read callback. 0 = unlimited.
  // mvfst default is 1; moqx was hardcoded to 10.
  uint16_t maxServerRecvPacketsPerLoop{64};
  // Number of UDP GRO buffers. 1 = disabled. > 1 enables kernel GRO coalescing.
  uint32_t numGROBuffers{1};
  // Ignore the peer's path MTU and send full-size (max UDP payload) packets.
  // Useful for testing / controlled networks; off by default to respect PMTU.
  bool canIgnorePathMTU{false};

  // BBR (BBR1) congestion control tunables.
  // conservativeRecovery is shared with BBR2.
  struct BBR {
    bool conservativeRecovery{false}; // also applies to BBR2
    bool largeProbeRttCwnd{false};
    bool enableAckAggregationInStartup{false};
    bool probeRttDisabledIfAppLimited{false};
    bool drainToTarget{false};
  };

  // BBR2 congestion control tunables (BBR2-specific fields only;
  // shared fields like conservativeRecovery live in bbr:).
  struct BBR2 {
    bool ignoreInflightLongTerm{false};
    bool ignoreShortTerm{false};
    bool exitStartupOnLoss{true};
    bool enableRecoveryInStartup{true};
    bool enableRecoveryInProbeStates{true};
    bool enableRenoCoexistence{false};
    bool paceInitCwnd{false};
    float overrideCruisePacingGain{-1.0f};  // -1 = use BBR2 default
    float overrideCruiseCwndGain{-1.0f};    // -1 = use BBR2 default
    float overrideStartupPacingGain{-1.0f}; // -1 = use BBR2 default
    float overrideBwShortBeta{0.0f};
  };

  // Cubic congestion control tunables.
  struct Cubic {
    bool additiveIncreaseAfterHystart{false};
    bool onlyGrowCwndWhenLimited{false};
    bool leaveHeadroomForCwndLimited{false};
  };

  // Copa congestion control tunables.
  struct Copa {
    // Target utilisation sensitivity (moqx default: 0.05).
    double deltaParam{0.05};
  };

  // L4S tunables (usable alongside any ECN-aware CC algorithm).
  struct L4S {
    float ceTarget{0.0f}; // 0 = disabled
  };

  // UDP socket send/receive buffer size in bytes. 0 = use MoQServer default (1 MB).
  // Increase when high fan-out causes EAGAIN bursts under load.
  uint64_t udpSocketBufferBytes{0};

  BBR bbr;
  BBR2 bbr2;
  Cubic cubic;
  Copa copa;
  L4S l4s;
};

struct ListenerConfig {
  std::string name;
  folly::SocketAddress address;
  TlsMode tlsMode;
  std::string endpoint;
  std::string moqtVersions; // comma-separated string
  QuicStack quicStack{QuicStack::Mvfst};
  QuicConfig quic;   // merged from listener_defaults.quic + per-listener quic override
  MvfstConfig mvfst; // merged from listener_defaults.mvfst + per-listener mvfst override
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

struct AuthConfig {
  struct HmacKey {
    std::string id;
    std::string secret;
  };

  bool enabled{false};
  uint64_t tokenType{0};
  std::vector<HmacKey> hmacKeys;
  bool requireSetupToken{true};
  bool allowRequestTokenOverride{true};
  bool strictClaims{false};
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
  AuthConfig auth;
};

struct AdminConfig {
  folly::SocketAddress address;
  std::optional<TlsConfig> tls;
};

struct MLogConfig {
  std::string dir;        // output directory; empty = disabled
  float sampleRate{1.0f}; // 1.0 = log all sessions, 0.0 = none
  std::optional<uint32_t> maxAgeDays; // delete files older than N days; nullopt = no limit
  std::optional<uint64_t> maxDirMb;   // trim directory to this size in MB; nullopt = no limit
  uint32_t cleanupIntervalSecs{600};  // how often to run cleanup (default 10 min)
};

struct LoggingConfig {
  std::optional<MLogConfig> mlog;
};

struct Config {
  std::vector<ListenerConfig> listeners;
  folly::F14FastMap<std::string, ServiceConfig> services;
  std::optional<AdminConfig> admin;
  std::string relayID; // always set: from config or randomly generated
  uint32_t threads{1};
  bool useRelayThread{true};
  bool useLocalForwarders{false};
  bool mvfstBpfSteering{true};
  std::optional<LoggingConfig> logging;
};

} // namespace openmoq::moqx::config
