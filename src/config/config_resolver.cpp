#include "o_rly/config/loader/config_resolver.h"

#include <folly/String.h>

#include "o_rly/tls/tls_provider_registry.h"

namespace openmoq::o_rly::config {

namespace {

std::string moqtVersionsToString(const ParsedListenerConfig& listener) {
  if (!listener.moqt_versions.value().has_value() || listener.moqt_versions.value()->empty()) {
    return "";
  }
  return folly::join(',', *listener.moqt_versions.value());
}

void validateAdminTlsConfig(const ParsedAdminTlsConfig& tls, std::vector<std::string>& errors) {
  bool hasCert = tls.cert_file.value().has_value() && !tls.cert_file.value()->empty();
  bool hasKey = tls.key_file.value().has_value() && !tls.key_file.value()->empty();
  if (!hasCert || !hasKey) {
    errors.push_back("admin.tls: cert_file and key_file are required");
  }
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

folly::Expected<ResolvedConfig, std::string>
resolveConfig(const ParsedConfig& config, const tls::TlsProviderRegistry& registry) {
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

  // TLS: extract type string from variant, look up factory, invoke it
  std::shared_ptr<tls::TlsCertProvider> tlsProvider;
  listener.tls.visit([&](const auto& variant) {
    using T = std::decay_t<decltype(variant)>;
    auto type = typename T::Tag{}.name();

    auto* factory = registry.getFactory(type);
    if (!factory) {
      errors.push_back(
          "Listener '" + listener.name.value() + "': unknown TLS type '" + type + "'"
      );
      return;
    }
    auto result = (*factory)(listener.tls);
    if (result.hasError()) {
      errors.push_back(
          "Listener '" + listener.name.value() + "': " + result.error()
      );
      return;
    }
    tlsProvider = std::move(result.value());
  });

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

  // Resolve cache
  CacheConfig cacheConfig{
      .maxCachedTracks = cache.enabled.value() ? static_cast<size_t>(cache.max_tracks.value()) : 0,
      .maxCachedGroupsPerTrack = static_cast<size_t>(cache.max_groups_per_track.value()),
  };

  // Resolve listener
  ListenerConfig resolvedListener{
      .name = listener.name.value(),
      .address = folly::SocketAddress(sock.address.value(), sock.port.value()),
      .tlsProvider = std::move(tlsProvider),
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
