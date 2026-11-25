/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "config/loader/ConfigInit.h"

#include "config/loader/ConfigResolver.h"
#include "config/loader/Loader.h"

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

  auto result = loadAndResolveConfig(configPath, strictConfig);
  if (result.hasError()) {
    const auto& errors = result.error();
    std::cerr << "Error: " << errors.front();
    for (size_t i = 1; i < errors.size(); ++i) {
      std::cerr << "\n  - " << errors[i];
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

folly::Expected<ResolvedConfig, std::vector<std::string>>
loadAndResolveConfig(std::string_view configPath, bool strictConfig) {
  ParsedConfig parsed;
  try {
    parsed = loadConfig(std::string(configPath), strictConfig);
  } catch (const std::exception& e) {
    return folly::makeUnexpected(std::vector<std::string>{e.what()});
  }

  auto result = resolveConfig(parsed);
  if (result.hasError()) {
    return folly::makeUnexpected(std::vector<std::string>{result.error()});
  }

  if (strictConfig && !result.value().warnings.empty()) {
    std::vector<std::string> errors{"Config warnings treated as errors (--strict_config):"};
    for (auto& w : result.value().warnings) {
      errors.push_back(std::move(w));
    }
    return folly::makeUnexpected(std::move(errors));
  }
  return std::move(result.value());
}

} // namespace openmoq::moqx::config
