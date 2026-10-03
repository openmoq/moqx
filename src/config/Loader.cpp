/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "config/loader/Loader.h"
#include "config/loader/YamlDuplicateKeys.h"

#include <stdexcept>
#include <string>
#include <vector>

#include <folly/FileUtil.h>
#include <rfl/json.hpp>
#include <rfl/yaml.hpp>
#include <yaml-cpp/yaml.h>

namespace openmoq::moqx::config {

ParsedListeners::ParsedListeners(ReflectionType listeners) {
  listeners.visit([this](auto& alt) {
    using Alt = std::decay_t<decltype(alt)>;
    if constexpr (std::is_same_v<Alt, std::vector<ParsedDeprecatedListenerListEntry>>) {
      for (size_t i = 0; i < alt.size(); ++i) {
        if (alt[i].name.value().empty()) {
          throw std::invalid_argument(
              "listeners[" + std::to_string(i) + "]: name must not be empty"
          );
        }
        push_back({{std::move(alt[i].listener.get())}, std::move(alt[i].name.value())});
      }
    } else {
      for (auto& [key, listener] : alt) {
        if (key.empty()) {
          throw std::invalid_argument("listeners: a listener name must not be empty");
        }
        push_back({{std::move(listener)}, key});
      }
    }
  });
}

ParsedListeners::ReflectionType ParsedListeners::reflection() const {
  rfl::Object<ParsedListenerEntry> entries;
  for (const auto& listener : *this) {
    entries.insert(listener.name.value(), static_cast<const ParsedListenerEntry&>(listener));
  }
  return entries;
}

ParsedConfig loadConfig(const std::string& path, bool strict) {
  std::string content;
  if (!folly::readFile(path.c_str(), content)) {
    throw std::runtime_error("Failed to open config file '" + path + "'");
  }

  try {
    rejectDuplicateKeys(content);
  } catch (const YAML::Exception&) {
    // Malformed YAML: the reflect step below reports it.
  } catch (const std::runtime_error& e) {
    throw std::runtime_error("Failed to parse config file '" + path + "': " + e.what());
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
