/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "plugin/Registry.h"

namespace openmoq::moqx::plugin {

rfl::Generic mergeGeneric(rfl::Generic base, const rfl::Generic& overlay) {
  auto* baseObject = std::get_if<rfl::Generic::Object>(&base.variant());
  const auto* overlayObject = std::get_if<rfl::Generic::Object>(&overlay.variant());
  if (!baseObject || !overlayObject) {
    return overlay;
  }
  for (const auto& [key, value] : *overlayObject) {
    auto it = std::find_if(baseObject->begin(), baseObject->end(), [&](const auto& entry) {
      return entry.first == key;
    });
    if (it == baseObject->end()) {
      baseObject->insert(key, value);
    } else {
      it->second = mergeGeneric(std::move(it->second), value);
    }
  }
  return base;
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
