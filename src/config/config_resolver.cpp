#include "o_rly/config/loader/config_resolver.h"

#include <unordered_set>

#include <folly/String.h>

namespace openmoq::o_rly::config {

namespace {

// Error-message prefix for a match entry: "Service 'name' match[j]"
std::string matchPrefix(const std::string& name, size_t j) {
  return "Service '" + name + "' match[" + std::to_string(j) + "]";
}

std::string moqtVersionsToString(const ParsedListenerConfig& listener) {
  if (!listener.moqt_versions.value().has_value() || listener.moqt_versions.value()->empty()) {
    return "";
  }
  return folly::join(',', *listener.moqt_versions.value());
}

// Merge two cache configs: overlay takes precedence over base, field by field.
ParsedCacheConfig
mergeCacheConfigs(const ParsedCacheConfig& base, const ParsedCacheConfig& overlay) {
  ParsedCacheConfig merged;
  merged.enabled =
      overlay.enabled.value().has_value() ? overlay.enabled.value() : base.enabled.value();
  merged.max_tracks =
      overlay.max_tracks.value().has_value() ? overlay.max_tracks.value() : base.max_tracks.value();
  merged.max_groups_per_track = overlay.max_groups_per_track.value().has_value()
                                    ? overlay.max_groups_per_track.value()
                                    : base.max_groups_per_track.value();
  return merged;
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

CacheConfig resolveCacheConfig(const ParsedCacheConfig& cache) {
  // All fields must be present after merging (validated earlier).
  return CacheConfig{
      .maxCachedTracks =
          *cache.enabled.value() ? static_cast<size_t>(*cache.max_tracks.value()) : 0,
      .maxCachedGroupsPerTrack = static_cast<size_t>(*cache.max_groups_per_track.value()),
  };
}

// Encode an authority + path pair as a composite key for duplicate detection.
std::string makeCompositeKey(
    const std::string& authorityType,
    const std::string& authorityValue,
    const ParsedServiceConfig::MatchRule::PathMatch& path
) {
  std::string key = authorityType + ":" + authorityValue + "|";
  path.visit([&](const auto& alt) {
    using P = std::decay_t<decltype(alt)>;
    if constexpr (std::is_same_v<P, ParsedServiceConfig::MatchRule::ExactPath>) {
      key += "exact:" + alt.exact.value();
    } else {
      key += "prefix:" + alt.prefix.value();
    }
  });
  return key;
}

// --- Listener validation ---

void validateListener(
    const ParsedListenerConfig& listener,
    std::vector<std::string>& errors,
    std::vector<std::string>& warnings
) {
  const auto& sock = listener.udp.value().socket.value();
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
}

// --- Admin validation ---

void validateAdmin(const ParsedConfig& config, std::vector<std::string>& errors) {
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
}

// --- Match-entry validation helpers ---

struct AuthorityInfo {
  std::string type;  // "exact", "wildcard", or "any"
  std::string value; // authority value (empty for "any")
};

// Returns nullopt if the authority is invalid.
std::optional<AuthorityInfo> validateAuthority(
    const ParsedServiceConfig::MatchRule::AuthorityMatch& authority,
    const std::string& prefix,
    std::vector<std::string>& errors
) {
  std::optional<AuthorityInfo> result;

  authority.visit([&](const auto& alt) {
    using A = std::decay_t<decltype(alt)>;
    if constexpr (std::is_same_v<A, ParsedServiceConfig::MatchRule::ExactAuthority>) {
      if (alt.exact.value().empty()) {
        errors.push_back(prefix + ".authority: exact value must be non-empty");
      } else {
        result = AuthorityInfo{"exact", alt.exact.value()};
      }
    } else if constexpr (std::is_same_v<A, ParsedServiceConfig::MatchRule::WildcardAuthority>) {
      const auto& pat = alt.wildcard.value();
      if (pat.size() < 3 || pat[0] != '*' || pat[1] != '.') {
        errors.push_back(
            prefix + ".authority: wildcard must start with '*.' (e.g. '*.example.com'), got '" +
            pat + "'"
        );
      } else if (pat.substr(2).find('*') != std::string::npos) {
        errors.push_back(
            prefix + ".authority: wildcard must contain exactly one '*' at the start, got '" + pat +
            "'"
        );
      } else {
        result = AuthorityInfo{"wildcard", pat};
      }
    } else { // AnyAuthority
      if (!alt.any.value()) {
        errors.push_back(prefix + ".authority: any must be true");
      } else {
        result = AuthorityInfo{"any", ""};
      }
    }
  });

  return result;
}

// Returns false if the path is invalid.
bool validatePath(
    const ParsedServiceConfig::MatchRule::PathMatch& path,
    const std::string& prefix,
    std::vector<std::string>& errors
) {
  bool valid = true;
  path.visit([&](const auto& alt) {
    using P = std::decay_t<decltype(alt)>;
    const std::string& pathVal = [&]() -> const std::string& {
      if constexpr (std::is_same_v<P, ParsedServiceConfig::MatchRule::ExactPath>) {
        return alt.exact.value();
      } else {
        return alt.prefix.value();
      }
    }();

    if (pathVal.empty()) {
      errors.push_back(prefix + ".path: value must be non-empty");
      valid = false;
    } else if (pathVal[0] != '/') {
      errors.push_back(prefix + ".path: value must start with '/', got '" + pathVal + "'");
      valid = false;
    }
  });
  return valid;
}

// --- Service validation ---

void validateService(
    const ParsedServiceConfig& svc,
    const ParsedConfig& config,
    std::unordered_set<std::string>& serviceNames,
    std::unordered_set<std::string>& compositeKeys,
    std::vector<ParsedCacheConfig>& mergedCaches,
    std::vector<std::string>& errors
) {
  const auto& name = svc.name.value();

  if (!serviceNames.insert(name).second) {
    errors.push_back("Duplicate service name: '" + name + "'");
  }

  // Validate each match entry
  const auto& matchEntries = svc.match.value();
  for (size_t j = 0; j < matchEntries.size(); ++j) {
    const auto& entry = matchEntries[j];
    auto prefix = matchPrefix(name, j);

    auto authInfo = validateAuthority(entry.authority.value(), prefix, errors);
    if (!authInfo) {
      continue;
    }

    if (!validatePath(entry.path.value(), prefix, errors)) {
      continue;
    }

    auto key = makeCompositeKey(authInfo->type, authInfo->value, entry.path.value());
    if (!compositeKeys.insert(key).second) {
      errors.push_back("Duplicate (authority, path) combination in service '" + name + "': " + key);
    }
  }

  // Cache resolution: merge service_defaults.cache with service.cache (field by field).
  const ParsedCacheConfig* defaults = nullptr;
  if (config.service_defaults.value().has_value() &&
      config.service_defaults.value()->cache.value().has_value()) {
    defaults = &(*config.service_defaults.value()->cache.value());
  }

  ParsedCacheConfig merged;
  if (svc.cache.value().has_value() && defaults) {
    merged = mergeCacheConfigs(*defaults, *svc.cache.value());
  } else if (svc.cache.value().has_value()) {
    merged = *svc.cache.value();
  } else if (defaults) {
    merged = *defaults;
  }
  // else: merged has all-nullopt fields

  // Validate that all fields are present after merging.
  if (!merged.enabled.value().has_value()) {
    errors.push_back("Service '" + name + "': cache.enabled is required");
  }
  if (!merged.max_tracks.value().has_value()) {
    errors.push_back("Service '" + name + "': cache.max_tracks is required");
  }
  if (!merged.max_groups_per_track.value().has_value()) {
    errors.push_back("Service '" + name + "': cache.max_groups_per_track is required");
  }

  if (merged.enabled.value().has_value() && *merged.enabled.value() &&
      merged.max_groups_per_track.value().has_value() && *merged.max_groups_per_track.value() < 1) {
    errors.push_back(
        "Service '" + name + "': cache.max_groups_per_track must be >= 1 when cache is enabled"
    );
  }

  mergedCaches.push_back(std::move(merged));
}

// --- Resolution helpers ---

ListenerConfig resolveListener(const ParsedListenerConfig& listener) {
  const auto& sock = listener.udp.value().socket.value();
  const auto& tls = listener.tls.value();

  TlsMode tlsMode;
  if (tls.insecure.value()) {
    tlsMode = Insecure{};
  } else {
    tlsMode = resolveTlsConfig(tls);
  }

  return ListenerConfig{
      .name = listener.name.value(),
      .address = folly::SocketAddress(sock.address.value(), sock.port.value()),
      .tlsMode = std::move(tlsMode),
      .endpoint = listener.endpoint.value(),
      .moqtVersions = moqtVersionsToString(listener),
  };
}

ServiceConfig::MatchEntry resolveMatchEntry(const ParsedServiceConfig::MatchRule& entry) {
  using ME = ServiceConfig::MatchEntry;
  std::variant<ME::ExactAuthority, ME::WildcardAuthority, ME::AnyAuthority> resolvedAuth;
  entry.authority.value().visit([&](const auto& alt) {
    using A = std::decay_t<decltype(alt)>;
    if constexpr (std::is_same_v<A, ParsedServiceConfig::MatchRule::ExactAuthority>) {
      resolvedAuth = ME::ExactAuthority{alt.exact.value()};
    } else if constexpr (std::is_same_v<A, ParsedServiceConfig::MatchRule::WildcardAuthority>) {
      resolvedAuth = ME::WildcardAuthority{alt.wildcard.value()};
    } else {
      resolvedAuth = ME::AnyAuthority{};
    }
  });

  ME::PathMatcher resolvedPath;
  entry.path.value().visit([&](const auto& alt) {
    using P = std::decay_t<decltype(alt)>;
    if constexpr (std::is_same_v<P, ParsedServiceConfig::MatchRule::ExactPath>) {
      resolvedPath = ME::ExactPath{alt.exact.value()};
    } else {
      resolvedPath = ME::PrefixPath{alt.prefix.value()};
    }
  });

  return ME{
      .authority = std::move(resolvedAuth),
      .path = std::move(resolvedPath),
  };
}

ServiceConfig resolveService(const ParsedServiceConfig& svc, const ParsedCacheConfig& cache) {
  std::vector<ServiceConfig::MatchEntry> entries;
  for (const auto& entry : svc.match.value()) {
    entries.push_back(resolveMatchEntry(entry));
  }
  return ServiceConfig{
      .name = svc.name.value(),
      .match = std::move(entries),
      .cache = resolveCacheConfig(cache),
  };
}

} // namespace

folly::Expected<ResolvedConfig, std::string> resolveConfig(const ParsedConfig& config) {
  std::vector<std::string> errors;
  std::vector<std::string> warnings;

  // === Validate listeners ===

  if (config.listeners.value().empty()) {
    return folly::makeUnexpected(std::string("At least one listener is required"));
  }
  if (config.listeners.value().size() > 1) {
    return folly::makeUnexpected(
        "Currently only one listener is supported, got " +
        std::to_string(config.listeners.value().size())
    );
  }

  const auto& listener = config.listeners.value()[0];
  validateListener(listener, errors, warnings);

  // === Validate admin ===
  validateAdmin(config, errors);

  // === Validate services ===

  const auto& services = config.services.value();
  if (services.empty()) {
    errors.push_back("At least one service is required");
  }

  std::unordered_set<std::string> serviceNames;
  std::unordered_set<std::string> compositeKeys;
  std::vector<ParsedCacheConfig> mergedCaches;

  for (const auto& svc : services) {
    validateService(svc, config, serviceNames, compositeKeys, mergedCaches, errors);
  }

  if (!errors.empty()) {
    return folly::makeUnexpected("Config validation failed:\n  - " + folly::join("\n  - ", errors));
  }

  // === Resolve ===

  std::vector<ServiceConfig> resolvedServices;
  resolvedServices.reserve(services.size());
  for (size_t i = 0; i < services.size(); ++i) {
    resolvedServices.push_back(resolveService(services[i], mergedCaches[i]));
  }

  // Resolve admin config
  const auto& adminOptional = config.admin.value();
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
              .listener = resolveListener(listener),
              .services = std::move(resolvedServices),
              .admin = std::move(adminConfig),
          },
      .warnings = std::move(warnings),
  };
}

} // namespace openmoq::o_rly::config
