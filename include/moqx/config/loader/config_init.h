#pragma once

#include <string>
#include <string_view>

#include <folly/Expected.h>

#include "moqx/config/resolved_config.h"

namespace openmoq::moqx::config {

constexpr std::string_view kDumpConfigSchemaCommand = "dump-config-schema";
constexpr std::string_view kValidateConfigCommand = "validate-config";

/// Returns usage lines for config-specific subcommands.
std::string configSubcommandUsage();

/// Handle config-specific subcommands or load+resolve config.
/// Returns ResolvedConfig (with warnings) or an exit code.
folly::Expected<ResolvedConfig, int> handleConfigSubcommand(
    std::string_view subcommand,
    std::string_view configPath,
    bool strictConfig,
    const char* programName
);

} // namespace openmoq::moqx::config
