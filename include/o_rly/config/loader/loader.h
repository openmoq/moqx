#pragma once

#include <string>

#include "o_rly/config/loader/parsed_config.h"

namespace openmoq::o_rly::config {

/// Load config from a YAML file path.
/// When strict=true, rejects unknown fields.
/// Throws std::runtime_error on parse failure.
/// Does NOT validate semantics — call resolveConfig() on the result.
ParsedConfig loadConfig(const std::string& path, bool strict = false);

/// Generate JSON schema string from config structs.
std::string generateSchema();

} // namespace openmoq::o_rly::config
