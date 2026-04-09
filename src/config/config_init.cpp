#include "config/loader/config_init.h"

#include "config/loader/config_resolver.h"
#include "config/loader/loader.h"

#include <gflags/gflags.h>

#include <iostream>
#include <string>

namespace openmoq::moqx::config {

std::string configSubcommandUsage() {
  return "  validate-config      Load and validate config, then exit\n"
         "  dump-config-schema   Print JSON schema to stdout, then exit\n";
}

folly::Expected<ResolvedConfig, int> handleConfigSubcommand(
    std::string_view subcommand,
    std::string_view configPath,
    bool strictConfig,
    const char* programName
) {
  if (subcommand == kDumpConfigSchemaCommand) {
    std::cout << generateSchema() << '\n';
    return folly::makeUnexpected(0);
  }

  if (configPath.empty()) {
    std::cerr << "Error: --config is required\n";
    google::ShowUsageWithFlags(programName);
    return folly::makeUnexpected(1);
  }

  ParsedConfig parsed;
  try {
    parsed = loadConfig(std::string(configPath), strictConfig);
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return folly::makeUnexpected(1);
  }

  auto result = resolveConfig(parsed);
  if (result.hasError()) {
    std::cerr << "Error: " << result.error() << std::endl;
    return folly::makeUnexpected(1);
  }

  if (strictConfig && !result.value().warnings.empty()) {
    std::cerr << "Error: Config warnings treated as errors (--strict_config):";
    for (const auto& w : result.value().warnings) {
      std::cerr << "\n  - " << w;
    }
    std::cerr << std::endl;
    return folly::makeUnexpected(1);
  }

  if (subcommand == kValidateConfigCommand) {
    for (const auto& w : result.value().warnings) {
      std::cerr << "Warning: " << w << std::endl;
    }
    std::cout << "Config is valid." << '\n';
    return folly::makeUnexpected(0);
  }

  return std::move(result.value());
}

} // namespace openmoq::moqx::config
