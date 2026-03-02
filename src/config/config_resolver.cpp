#include "o_rly/config/config_resolver.h"

#include <cassert>

#include <folly/String.h>

namespace openmoq::o_rly::config {

namespace {

std::string moqtVersionsToString(const ParsedListenerConfig& listener) {
  if (!listener.moqt_versions.value().has_value() || listener.moqt_versions.value()->empty()) {
    return "";
  }
  return folly::join(',', *listener.moqt_versions.value());
}

} // namespace

ConfigDiagnostics diagnoseConfig(const ParsedConfig& config) {
  ConfigDiagnostics diag;

  // Exactly one listener required
  if (config.listeners.value().empty()) {
    diag.errors.push_back("At least one listener is required");
    return diag;
  }
  if (config.listeners.value().size() > 1) {
    diag.errors.push_back(
        "Currently only one listener is supported, got " +
        std::to_string(config.listeners.value().size())
    );
    return diag;
  }

  const auto& listener = config.listeners.value()[0];

  // Listener must have udp configured
  if (!listener.udp.value().has_value()) {
    diag.errors.push_back(
        "Listener '" + listener.name.value() + "' must have 'udp' transport configured"
    );
  } else {
    // Port validation
    auto sock = listener.udp.value()->socketOrDefault();
    uint16_t port = sock.portOrDefault();
    if (port == 0) {
      diag.errors.push_back("Listener '" + listener.name.value() + "' port must be 1-65535, got 0");
    }
  }

  // TLS validation
  if (listener.tls.value().has_value()) {
    const auto& tls = *listener.tls.value();
    if (!tls.insecureOrDefault()) {
      bool hasCert = tls.cert_file.value().has_value() && !tls.cert_file.value()->empty();
      bool hasKey = tls.key_file.value().has_value() && !tls.key_file.value()->empty();
      if (!hasCert || !hasKey) {
        diag.errors.push_back(
            "Listener '" + listener.name.value() +
            "': cert_file and key_file are required when insecure=false"
        );
      }
    } else {
      // Warn if certs provided with insecure=true
      bool hasCert = tls.cert_file.value().has_value() && !tls.cert_file.value()->empty();
      bool hasKey = tls.key_file.value().has_value() && !tls.key_file.value()->empty();
      if (hasCert || hasKey) {
        diag.warnings.push_back(
            "Listener '" + listener.name.value() +
            "': cert_file/key_file are ignored when insecure=true"
        );
      }
    }
  } else {
    // No tls config at all — default insecure=false requires certs
    diag.errors.push_back(
        "Listener '" + listener.name.value() +
        "': tls is required (set insecure: true for "
        "plaintext mode)"
    );
  }

  // Cache validation
  auto cache = config.cacheOrDefault();
  if (cache.enabledOrDefault()) {
    if (cache.maxGroupsPerTrackOrDefault() < 1) {
      diag.errors.push_back("cache.max_groups_per_track must be >= 1 when cache is enabled");
    }
  }

  return diag;
}

Config resolveConfig(const ParsedConfig& config) {
  assert(
      config.listeners.value().size() == 1 && "resolveConfig requires exactly one listener; "
                                              "call diagnoseConfig() first"
  );
  const auto& parsedListener = config.listeners.value()[0];
  assert(parsedListener.udp.value().has_value() && "listener must have udp configured");
  auto parsedCache = config.cacheOrDefault();
  auto sock = parsedListener.udp.value()->socketOrDefault();

  // Resolve TLS mode
  TlsMode tlsMode;
  const auto& tlsOpt = parsedListener.tls.value();
  if (!tlsOpt.has_value() || tlsOpt->insecureOrDefault()) {
    tlsMode = Insecure{};
  } else {
    tlsMode = TlsConfig{
        .certFile = tlsOpt->cert_file.value().value_or(""),
        .keyFile = tlsOpt->key_file.value().value_or(""),
    };
  }

  // Resolve cache
  CacheConfig cache{
      .maxCachedTracks = parsedCache.enabledOrDefault()
                             ? static_cast<size_t>(parsedCache.maxTracksOrDefault())
                             : 0,
      .maxCachedGroupsPerTrack = static_cast<size_t>(parsedCache.maxGroupsPerTrackOrDefault()),
  };

  // Resolve listener
  ListenerConfig listener{
      .name = parsedListener.name.value(),
      .address = folly::SocketAddress(sock.addressOrDefault(), sock.portOrDefault()),
      .tlsMode = std::move(tlsMode),
      .endpoint = parsedListener.endpointOrDefault(),
      .moqtVersions = moqtVersionsToString(parsedListener),
  };

  return Config{
      .listener = std::move(listener),
      .cache = cache,
  };
}

} // namespace openmoq::o_rly::config
