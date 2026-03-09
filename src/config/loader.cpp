#include "o_rly/config/loader/loader.h"

#include <stdexcept>

#include <folly/FileUtil.h>
#include <rfl/json.hpp>
#include <rfl/yaml.hpp>

namespace openmoq::o_rly::config {

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
      rfl::Description<"Configuration schema for the o-rly relay.", ParsedConfig>>(
      rfl::json::pretty
  );
}

} // namespace openmoq::o_rly::config
