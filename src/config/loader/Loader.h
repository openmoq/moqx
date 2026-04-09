/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>

#include "config/loader/ParsedConfig.h"

namespace openmoq::moqx::config {

/// Load config from a YAML file path.
/// When strict=true, rejects unknown fields.
/// Throws std::runtime_error on parse failure.
/// Does NOT validate semantics — call resolveConfig() on the result.
ParsedConfig loadConfig(const std::string& path, bool strict = false);

/// Generate JSON schema string from config structs.
std::string generateSchema();

} // namespace openmoq::moqx::config
