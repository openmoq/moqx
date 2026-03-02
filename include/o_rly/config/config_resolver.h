#pragma once

#include <string>
#include <vector>

#include "o_rly/config/config.h"
#include "o_rly/config/parsed_config.h"

namespace openmoq::o_rly::config {

struct ConfigDiagnostics {
  std::vector<std::string> errors;
  std::vector<std::string> warnings;
};

/// Validate and warn on a parsed config. Merges semantic validation (errors)
/// and advisory checks (warnings) into a single result.
ConfigDiagnostics diagnoseConfig(const ParsedConfig& config);

/// Convert a validated ParsedConfig into a resolved Config with concrete types.
/// Assumes diagnoseConfig() returned no errors.
Config resolveConfig(const ParsedConfig& config);

} // namespace openmoq::o_rly::config
