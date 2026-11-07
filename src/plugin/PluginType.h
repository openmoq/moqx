/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "plugin/Api.h"

#include <folly/Expected.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace YAML {
class Node;
} // namespace YAML

namespace openmoq::moqx::config {
class ConfigSink;
} // namespace openmoq::moqx::config

namespace openmoq::moqx::plugin {

enum class Scope { Process, Service };

// A registered plugin type. Build one with makePluginType (plugin/Registry.h).
// The typed config travels type-erased; only this type's functions read it.
struct PluginType {
  std::string name;
  bool processScope{false};
  bool serviceScope{false};
  // Top-level config field names, checked against service_defaults.plugins.
  std::vector<std::string> fieldNames;
  // Parses the entry's `config` subtree as written. Errors name the field.
  std::function<folly::Expected<std::shared_ptr<const void>, std::string>(
      const YAML::Node& config,
      bool strict
  )>
      parse;
  // No side effects: --validate calls it and drops the result.
  std::function<Result<std::unique_ptr<Root>>(const void* config)> create;
  // Writes the config with secrets redacted.
  std::function<void(const void* config, config::ConfigSink& sink)> serialize;

  bool allows(Scope scope) const { return scope == Scope::Process ? processScope : serviceScope; }
};

using Registry = std::vector<std::shared_ptr<const PluginType>>;

inline std::shared_ptr<const PluginType> findType(const Registry& registry, std::string_view name) {
  for (const auto& type : registry) {
    if (type->name == name) {
      return type;
    }
  }
  return nullptr;
}

} // namespace openmoq::moqx::plugin
