/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "plugin/Registry.h"

namespace openmoq::moqx::plugin {

std::optional<std::string> nonStringKey(const YAML::Node& node) {
  if (node.IsMap()) {
    for (const auto& entry : node) {
      if (!entry.first.IsScalar()) {
        return YAML::Dump(entry.first);
      }
      if (auto bad = nonStringKey(entry.second)) {
        return bad;
      }
    }
  } else if (node.IsSequence()) {
    for (const auto& element : node) {
      if (auto bad = nonStringKey(element)) {
        return bad;
      }
    }
  }
  return std::nullopt;
}

// Callers check nonStringKey first: keys are scalars here.
YAML::Node mergeYaml(const YAML::Node& base, const YAML::Node& overlay) {
  if (!base.IsMap() || !overlay.IsMap()) {
    return YAML::Clone(overlay);
  }
  // YAML::Node copies share storage; clone so `base` stays as it was.
  YAML::Node merged = YAML::Clone(base);
  for (const auto& entry : overlay) {
    const auto& key = entry.first.Scalar();
    merged[key] = merged[key] ? mergeYaml(merged[key], entry.second) : YAML::Clone(entry.second);
  }
  return merged;
}

void writeGeneric(config::ConfigSink& sink, std::string_view key, const rfl::Generic& value) {
  std::visit(
      [&](const auto& v) {
        using V = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<V, bool>) {
          sink.boolField(key, v);
        } else if constexpr (std::is_same_v<V, int64_t>) {
          sink.intField(key, v);
        } else if constexpr (std::is_same_v<V, double>) {
          sink.doubleField(key, v);
        } else if constexpr (std::is_same_v<V, std::string>) {
          sink.stringField(key, v);
        } else if constexpr (std::is_same_v<V, rfl::Generic::Object>) {
          sink.beginObject(key);
          for (const auto& [name, field] : v) {
            writeGeneric(sink, name, field);
          }
          sink.endObject();
        } else if constexpr (std::is_same_v<V, rfl::Generic::Array>) {
          sink.beginArray(key);
          for (const auto& element : v) {
            writeGeneric(sink, "", element);
          }
          sink.endArray();
        } else {
          sink.nullField(key);
        }
      },
      value.variant()
  );
}

} // namespace openmoq::moqx::plugin
