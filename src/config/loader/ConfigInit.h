/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <folly/Expected.h>

#include "config/ResolvedConfig.h"

namespace openmoq::moqx::config {

constexpr std::string_view kDumpConfigSchemaCommand = "dump-config-schema";
constexpr std::string_view kValidateConfigCommand = "validate-config";

/// Returns usage lines for config-specific subcommands.
std::string configSubcommandUsage();

/// Handle config-specific subcommands or load+resolve config (via loadAndResolveConfig),
/// printing errors to stderr. Returns ResolvedConfig (with warnings) or an exit code.
folly::Expected<ResolvedConfig, int> handleConfigSubcommand(
    std::string_view subcommand,
    std::string_view configPath,
    bool strictConfig,
    const char* programName
);

/// Load, resolve, and (when strict) reject warnings. No printing, no exit.
/// On failure returns the error lines: a single message for load/resolve
/// failures, or a header followed by one entry per warning for the
/// strict-warnings rule.
folly::Expected<ResolvedConfig, std::vector<std::string>>
loadAndResolveConfig(std::string_view configPath, bool strictConfig);

} // namespace openmoq::moqx::config
