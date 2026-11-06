/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <string_view>

namespace openmoq::moqx::config {

// Sink callbacks. An empty `key` means the value is an array element (rendered
// without a key); a non-empty `key` means it is a named object field.
class ConfigSink {
public:
  virtual ~ConfigSink() = default;

  virtual void beginObject(std::string_view key) = 0;
  virtual void endObject() = 0;
  virtual void beginArray(std::string_view key) = 0;
  virtual void endArray() = 0;

  virtual void stringField(std::string_view key, std::string_view value) = 0;
  virtual void boolField(std::string_view key, bool value) = 0;
  virtual void intField(std::string_view key, int64_t value) = 0;
  virtual void uintField(std::string_view key, uint64_t value) = 0;
  virtual void doubleField(std::string_view key, double value) = 0;
  virtual void nullField(std::string_view key) = 0;
};

} // namespace openmoq::moqx::config
