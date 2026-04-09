/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>

#include <folly/Expected.h>

#include "config/ResolvedConfig.h"
#include "config/loader/ParsedConfig.h"

namespace openmoq::moqx::config {

/// Validate and resolve a ParsedConfig into concrete Config types.
/// Returns warnings alongside the config on success.
/// On validation failure, returns a combined error string.
folly::Expected<ResolvedConfig, std::string> resolveConfig(const ParsedConfig& config);

} // namespace openmoq::moqx::config
