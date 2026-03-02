#include "o_rly/config/loader.h"

#include <fstream>
#include <regex>
#include <sstream>

#include <rfl/json.hpp>
#include <rfl/yaml.hpp>

namespace openmoq::o_rly::config {

namespace {

std::string readFileContents(const std::string& path) {
  std::ifstream ifs(path);
  if (!ifs) {
    throw std::runtime_error("Failed to open config file '" + path + "'");
  }
  std::ostringstream oss;
  oss << ifs.rdbuf();
  return oss.str();
}

} // namespace

std::vector<std::string> detectUnknownFields(const std::string& yamlContent) {
  auto result = rfl::yaml::read<Config, rfl::NoExtraFields>(yamlContent);
  if (result) {
    return {};
  }

  std::vector<std::string> warnings;
  std::string errStr = result.error().what();

  // Matches reflect-cpp v0.18.0 error format for NoExtraFields violations.
  // Update if upgrading reflect-cpp changes the error message format.
  std::regex fieldRegex(R"(Value named '([^']+)' not used)");
  std::sregex_iterator it(errStr.begin(), errStr.end(), fieldRegex);
  std::sregex_iterator end;
  for (; it != end; ++it) {
    warnings.push_back("Unknown config field: '" + (*it)[1].str() + "' (will be ignored)");
  }

  if (warnings.empty() && !errStr.empty()) {
    // Fallback: return the raw error if regex didn't match
    warnings.push_back("Unknown fields detected: " + errStr);
  }

  return warnings;
}

LoadResult loadConfig(const std::string& path, bool strict) {
  LoadResult lr;

  auto content = readFileContents(path);

  auto result = rfl::yaml::read<Config>(content);
  if (!result) {
    throw std::runtime_error("Failed to parse config file '" + path +
                             "': " + result.error().what());
  }
  lr.config = std::move(*result);

  auto unknownWarnings = detectUnknownFields(content);
  if (strict && !unknownWarnings.empty()) {
    std::ostringstream oss;
    oss << "Unknown fields in config file '" << path << "':";
    for (const auto& w : unknownWarnings) {
      oss << "\n  - " << w;
    }
    throw std::runtime_error(oss.str());
  }
  lr.warnings.insert(lr.warnings.end(), unknownWarnings.begin(), unknownWarnings.end());

  auto errors = validateConfig(lr.config);
  if (!errors.empty()) {
    std::ostringstream oss;
    oss << "Config validation failed:";
    for (const auto& err : errors) {
      oss << "\n  - " << err;
    }
    throw std::runtime_error(oss.str());
  }

  auto semanticWarnings = warnConfig(lr.config);
  lr.warnings.insert(lr.warnings.end(), semanticWarnings.begin(), semanticWarnings.end());
  return lr;
}

std::vector<std::string> validateConfig(const Config& config) {
  std::vector<std::string> errors;

  // Exactly one listener required
  if (config.listeners.value().empty()) {
    errors.push_back("At least one listener is required");
    return errors;
  }
  if (config.listeners.value().size() > 1) {
    errors.push_back("Currently only one listener is supported, got " +
                     std::to_string(config.listeners.value().size()));
  }

  const auto& listener = config.listeners.value()[0];

  // Listener must have udp configured
  if (!listener.udp.value().has_value()) {
    errors.push_back("Listener '" + listener.name.value() +
                     "' must have 'udp' transport configured");
  } else {
    // Port validation
    auto sock = listener.udp.value()->socketOrDefault();
    uint16_t port = sock.portOrDefault();
    if (port == 0) {
      errors.push_back("Listener '" + listener.name.value() + "' port must be 1-65535, got 0");
    }
  }

  // TLS validation
  if (listener.tls_credentials.value().has_value()) {
    const auto& tls = *listener.tls_credentials.value();
    if (!tls.insecureOrDefault()) {
      bool hasCert = tls.cert_file.value().has_value() && !tls.cert_file.value()->empty();
      bool hasKey = tls.key_file.value().has_value() && !tls.key_file.value()->empty();
      if (!hasCert || !hasKey) {
        errors.push_back("Listener '" + listener.name.value() +
                         "': cert_file and key_file are required when insecure=false");
      }
    }
  } else {
    // No tls_credentials at all — default insecure=false requires certs
    errors.push_back("Listener '" + listener.name.value() +
                     "': tls_credentials is required (set insecure: true for "
                     "plaintext mode)");
  }

  // Cache validation
  auto cache = config.cacheOrDefault();
  if (cache.enabledOrDefault()) {
    if (cache.maxGroupsPerTrackOrDefault() < 1) {
      errors.push_back("cache.max_groups_per_track must be >= 1 when cache is enabled");
    }
  }

  return errors;
}

std::vector<std::string> warnConfig(const Config& config) {
  std::vector<std::string> warnings;

  if (config.listeners.value().empty()) {
    return warnings;
  }

  const auto& listener = config.listeners.value()[0];
  if (listener.tls_credentials.value().has_value()) {
    const auto& tls = *listener.tls_credentials.value();
    if (tls.insecureOrDefault()) {
      bool hasCert = tls.cert_file.value().has_value() && !tls.cert_file.value()->empty();
      bool hasKey = tls.key_file.value().has_value() && !tls.key_file.value()->empty();
      if (hasCert || hasKey) {
        warnings.push_back("Listener '" + listener.name.value() +
                           "': cert_file/key_file are ignored when insecure=true");
      }
    }
  }

  return warnings;
}

std::string generateSchema() {
  return rfl::json::to_schema<
      rfl::Description<"Configuration schema for the o-rly relay.", Config>>(rfl::json::pretty);
}

std::string moqtVersionsToString(const ListenerConfig& listener) {
  if (!listener.moqt_versions.value().has_value() || listener.moqt_versions.value()->empty()) {
    return "";
  }
  std::string result;
  for (auto v : *listener.moqt_versions.value()) {
    if (!result.empty())
      result += ',';
    result += std::to_string(v);
  }
  return result;
}

} // namespace openmoq::o_rly::config
