#include "o_rly/config/loader/config_resolver.h"

#include <sstream>

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

folly::Expected<ResolvedConfig, std::string> resolveConfig(const ParsedConfig& config) {
  std::vector<std::string> errors;
  std::vector<std::string> warnings;

  // Exactly one listener required
  if (config.listeners.value().empty()) {
    errors.push_back("At least one listener is required");
    return folly::makeUnexpected(errors[0]);
  }
  if (config.listeners.value().size() > 1) {
    errors.push_back(
        "Currently only one listener is supported, got " +
        std::to_string(config.listeners.value().size())
    );
    return folly::makeUnexpected(errors[0]);
  }

  const auto& listener = config.listeners.value()[0];

  // Listener must have udp configured
  if (!listener.udp.value().has_value()) {
    errors.push_back(
        "Listener '" + listener.name.value() + "' must have 'udp' transport configured"
    );
  } else {
    // Port validation
    auto sock = listener.udp.value()->socketOrDefault();
    uint16_t port = sock.portOrDefault();
    if (port == 0) {
      errors.push_back("Listener '" + listener.name.value() + "' port must be 1-65535, got 0");
    }
  }

  // TLS validation
  if (listener.tls.value().has_value()) {
    const auto& tls = *listener.tls.value();
    if (!tls.insecureOrDefault()) {
      bool hasCert = tls.cert_file.value().has_value() && !tls.cert_file.value()->empty();
      bool hasKey = tls.key_file.value().has_value() && !tls.key_file.value()->empty();
      if (!hasCert || !hasKey) {
        errors.push_back(
            "Listener '" + listener.name.value() +
            "': cert_file and key_file are required when insecure=false"
        );
      }
    } else {
      // Warn if certs provided with insecure=true
      bool hasCert = tls.cert_file.value().has_value() && !tls.cert_file.value()->empty();
      bool hasKey = tls.key_file.value().has_value() && !tls.key_file.value()->empty();
      if (hasCert || hasKey) {
        warnings.push_back(
            "Listener '" + listener.name.value() +
            "': cert_file/key_file are ignored when insecure=true"
        );
      }
    }
  } else {
    // No tls config at all — default insecure=false requires certs
    errors.push_back(
        "Listener '" + listener.name.value() +
        "': tls is required (set insecure: true for "
        "plaintext mode)"
    );
  }

  // Cache validation
  auto cache = config.cacheOrDefault();
  if (cache.enabledOrDefault()) {
    if (cache.maxGroupsPerTrackOrDefault() < 1) {
      errors.push_back("cache.max_groups_per_track must be >= 1 when cache is enabled");
    }
  }

  if (!errors.empty()) {
    std::ostringstream oss;
    oss << "Config validation failed:";
    for (const auto& err : errors) {
      oss << "\n  - " << err;
    }
    return folly::makeUnexpected(oss.str());
  }

  // --- Resolve ---

  const auto& parsedListener = listener;
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
  CacheConfig cacheConfig{
      .maxCachedTracks = parsedCache.enabledOrDefault()
                             ? static_cast<size_t>(parsedCache.maxTracksOrDefault())
                             : 0,
      .maxCachedGroupsPerTrack = static_cast<size_t>(parsedCache.maxGroupsPerTrackOrDefault()),
  };

  // Resolve listener
  ListenerConfig resolvedListener{
      .name = parsedListener.name.value(),
      .address = folly::SocketAddress(sock.addressOrDefault(), sock.portOrDefault()),
      .tlsMode = std::move(tlsMode),
      .endpoint = parsedListener.endpointOrDefault(),
      .moqtVersions = moqtVersionsToString(parsedListener),
  };

  return ResolvedConfig{
      .config =
          Config{
              .listener = std::move(resolvedListener),
              .cache = cacheConfig,
          },
      .warnings = std::move(warnings),
  };
}

} // namespace openmoq::o_rly::config
