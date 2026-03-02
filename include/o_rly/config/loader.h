#pragma once

#include <string>
#include <vector>

#include "o_rly/config/config.h"

namespace openmoq::o_rly::config {

struct LoadResult {
  Config config;
  std::vector<std::string> warnings;
};

/// Load and validate config from a YAML file path.
/// When strict=true, throws on unknown fields; otherwise adds warnings.
/// Throws std::runtime_error on parse failure or validation error.
LoadResult loadConfig(const std::string& path, bool strict = false);

/// Detect unknown fields by parsing with rfl::NoExtraFields.
/// Returns list of human-readable warning strings (empty = clean).
std::vector<std::string> detectUnknownFields(const std::string& yamlContent);

/// Semantic validation beyond schema checks.
/// Returns error messages; empty means valid.
std::vector<std::string> validateConfig(const Config& config);

/// Semantic checks that produce warnings (non-fatal).
std::vector<std::string> warnConfig(const Config& config);

/// Generate JSON schema string from config structs.
std::string generateSchema();

/// Convert a listener's moqt_versions list to a comma-separated string
/// compatible with ORelayServer's versions parameter.
/// Returns "" if moqt_versions is not set.
std::string moqtVersionsToString(const ListenerConfig& listener);

} // namespace openmoq::o_rly::config
