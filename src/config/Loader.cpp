/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "config/loader/Loader.h"

#include <stdexcept>

#include <folly/FileUtil.h>
#include <rfl/json.hpp>
#include <rfl/yaml.hpp>

namespace openmoq::moqx::config {

ParsedConfig loadConfig(const std::string& path, bool strict) {
  std::string content;
  if (!folly::readFile(path.c_str(), content)) {
    throw std::runtime_error("Failed to open config file '" + path + "'");
  }

  auto result = strict ? rfl::yaml::read<ParsedConfig, rfl::NoExtraFields>(content)
                       : rfl::yaml::read<ParsedConfig>(content);
  if (!result) {
    throw std::runtime_error(
        "Failed to parse config file '" + path + "': " + result.error().what()
    );
  }
  return std::move(*result);
}

std::string generateSchema() {
  return rfl::json::to_schema<
      rfl::Description<"Configuration schema for the moqx relay.", ParsedConfig>>(rfl::json::pretty
  );
}

} // namespace openmoq::moqx::config
