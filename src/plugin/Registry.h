/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// Building a PluginType from a reflect-cpp config struct.
// - Parsing is typed; under --strict_config unknown fields are errors.
// - Serialization goes through `redact` on a copy, so the live config keeps
//   its secrets.

#include "config/ConfigSink.h"
#include "plugin/PluginType.h"

#include <rfl.hpp>
#include <rfl/yaml.hpp>
#include <yaml-cpp/yaml.h>

#include <functional>
#include <optional>
#include <string>

namespace openmoq::moqx::plugin {

// Deep-merges `overlay` into a copy of `base`: maps merge key by key,
// everything else is replaced by the overlay's value. Neither input changes.
YAML::Node mergeYaml(const YAML::Node& base, const YAML::Node& overlay);

// The first map key in `node`, at any depth, that is not a scalar; rendered
// as YAML.
std::optional<std::string> nonStringKey(const YAML::Node& node);

// Walks a generic value into the sink; an empty key renders an array element.
void writeGeneric(config::ConfigSink& sink, std::string_view key, const rfl::Generic& value);

struct Scopes {
  bool process{false};
  bool service{false};
};

template <class Config>
std::shared_ptr<const PluginType> makePluginType(
    std::string name,
    Scopes scopes,
    std::function<Result<std::unique_ptr<Root>>(const Config&)> factory,
    std::function<void(Config&)> redact = {}
) {
  auto type = std::make_shared<PluginType>();
  type->name = std::move(name);
  type->processScope = scopes.process;
  type->serviceScope = scopes.service;
  for (const auto& field : rfl::fields<Config>()) {
    type->fieldNames.push_back(field.name());
  }
  type->parse = [](const YAML::Node& node,
                   bool strict) -> folly::Expected<std::shared_ptr<const void>, std::string> {
    const auto var = rfl::yaml::InputVarType(node);
    auto parsed =
        strict ? rfl::yaml::read<Config, rfl::NoExtraFields>(var) : rfl::yaml::read<Config>(var);
    if (!parsed) {
      return folly::makeUnexpected(std::string(parsed.error().what()));
    }
    return std::shared_ptr<const void>(std::make_shared<const Config>(std::move(*parsed)));
  };
  type->create = [factory = std::move(factory)](const void* config) {
    return factory(*static_cast<const Config*>(config));
  };
  type->serialize = [redact = std::move(redact)](const void* config, config::ConfigSink& sink) {
    Config copy = *static_cast<const Config*>(config);
    if (redact) {
      redact(copy);
    }
    writeGeneric(sink, "config", rfl::to_generic(copy));
  };
  return type;
}

} // namespace openmoq::moqx::plugin
