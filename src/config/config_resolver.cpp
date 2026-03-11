#include "o_rly/config/loader/config_resolver.h"

#include <folly/String.h>

namespace openmoq::o_rly::config {

namespace {

std::string moqtVersionsToString(const ParsedListenerConfig& listener) {
  if (!listener.moqt_versions.value().has_value() || listener.moqt_versions.value()->empty()) {
    return "";
  }
  return folly::join(',', *listener.moqt_versions.value());
}

void validateListenerTlsConfig(
    const ParsedListenerTlsConfig& tls,
    std::string_view context,
    std::vector<std::string>& errors,
    std::vector<std::string>& warnings
) {
  bool hasCert = tls.cert_file.value().has_value() && !tls.cert_file.value()->empty();
  bool hasKey = tls.key_file.value().has_value() && !tls.key_file.value()->empty();
  if (!tls.insecure.value()) {
    if (!hasCert || !hasKey) {
      errors.push_back(
          std::string(context) + ": cert_file and key_file are required when insecure=false"
      );
    }
  } else if (hasCert || hasKey) {
    warnings.push_back(
        std::string(context) + ": cert_file/key_file are ignored when insecure=true"
    );
  }
}

void validateAdminTlsConfig(const ParsedAdminTlsConfig& tls, std::vector<std::string>& errors) {
  bool hasCert = tls.cert_file.value().has_value() && !tls.cert_file.value()->empty();
  bool hasKey = tls.key_file.value().has_value() && !tls.key_file.value()->empty();
  if (!hasCert || !hasKey) {
    errors.push_back("admin.tls: cert_file and key_file are required");
  }
}

TlsConfig resolveTlsConfig(const ParsedListenerTlsConfig& tls) {
  return TlsConfig{
      .certFile = tls.cert_file.value().value_or(""),
      .keyFile = tls.key_file.value().value_or(""),
      .alpn = {},
  };
}

TlsConfig resolveAdminTlsConfig(
    const ParsedAdminTlsConfig& tls,
    const std::vector<std::string>& defaultAlpn
) {
  return TlsConfig{
      .certFile = tls.cert_file.value().value_or(""),
      .keyFile = tls.key_file.value().value_or(""),
      .alpn = tls.alpn.value().value_or(defaultAlpn),
  };
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
  const auto& sock = listener.udp.value().socket.value();

  // Port validation
  if (sock.port.value() == 0) {
    errors.push_back("Listener '" + listener.name.value() + "' port must be 1-65535, got 0");
  }

  // TLS validation
  validateListenerTlsConfig(
      listener.tls.value(),
      "Listener '" + listener.name.value() + "'",
      errors,
      warnings
  );
  // Admin config validation
  const auto& adminOptional = config.admin.value();
  if (adminOptional.has_value()) {
    if (adminOptional->port.value() == 0) {
      errors.push_back("admin.port must be 1-65535, got 0");
    }
    bool hasTls = adminOptional->tls.value().has_value();
    bool hasPlaintext = adminOptional->plaintext.value();
    if (hasTls && hasPlaintext) {
      errors.push_back("admin: plaintext and tls are mutually exclusive");
    } else if (!hasTls && !hasPlaintext) {
      errors.push_back("admin: one of plaintext or tls must be set");
    }
    if (hasTls) {
      validateAdminTlsConfig(*adminOptional->tls.value(), errors);
    }
  }

  // Cache validation
  const auto& cache = config.cache.value();
  if (cache.enabled.value()) {
    if (cache.max_groups_per_track.value() < 1) {
      errors.push_back("cache.max_groups_per_track must be >= 1 when cache is enabled");
    }
  }

  if (!errors.empty()) {
    return folly::makeUnexpected("Config validation failed:\n  - " + folly::join("\n  - ", errors));
  }

  // --- Resolve ---

  // Resolve listener TLS mode
  const auto& tls = listener.tls.value();
  TlsMode tlsMode;
  if (tls.insecure.value()) {
    tlsMode = Insecure{};
  } else {
    tlsMode = resolveTlsConfig(tls);
  }

  // Resolve cache
  CacheConfig cacheConfig{
      .maxCachedTracks = cache.enabled.value() ? static_cast<size_t>(cache.max_tracks.value()) : 0,
      .maxCachedGroupsPerTrack = static_cast<size_t>(cache.max_groups_per_track.value()),
  };

  // Resolve listener
  ListenerConfig resolvedListener{
      .name = listener.name.value(),
      .address = folly::SocketAddress(sock.address.value(), sock.port.value()),
      .tlsMode = std::move(tlsMode),
      .endpoint = listener.endpoint.value(),
      .moqtVersions = moqtVersionsToString(listener),
  };

  // Resolve admin config
  std::optional<AdminConfig> adminConfig;
  if (adminOptional.has_value()) {
    static const std::vector<std::string> kDefaultAdminAlpn = {"h2", "http/1.1"};
    std::optional<TlsConfig> adminTls;
    if (adminOptional->tls.value().has_value()) {
      adminTls = resolveAdminTlsConfig(*adminOptional->tls.value(), kDefaultAdminAlpn);
    }
    adminConfig = AdminConfig{
        .address =
            folly::SocketAddress(adminOptional->address.value(), adminOptional->port.value()),
        .tls = std::move(adminTls),
    };
  }

  return ResolvedConfig{
      .config =
          Config{
              .listener = std::move(resolvedListener),
              .cache = cacheConfig,
              .admin = std::move(adminConfig),
          },
      .warnings = std::move(warnings),
  };
}

} // namespace openmoq::o_rly::config
