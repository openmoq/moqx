/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include <folly/io/Cursor.h>

namespace openmoq::moqx::admin {

// Minimal streaming JSON emitter backed by a folly::io::QueueAppender.
// No intermediate representation is built — bytes are written directly
// to the appender as each method is called.
//
// Comma state is tracked by a single instance, so the same JsonWriter
// must be shared across nested helper classes (e.g. a context visitor
// and its per-service relay visitor) to keep commas correct across layers.
class JsonWriter {
public:
  explicit JsonWriter(folly::io::QueueAppender& app) : app_(app) {}

  void beginObject() {
    maybeComma();
    append('{');
    needsComma_ = false;
  }
  void endObject() {
    append('}');
    needsComma_ = true;
  }
  void beginArray() {
    maybeComma();
    append('[');
    needsComma_ = false;
  }
  void endArray() {
    append(']');
    needsComma_ = true;
  }

  // Writes "key": — no comma after; caller writes the value immediately after.
  void key(std::string_view k) {
    maybeComma();
    writeString(k);
    append(':');
    needsComma_ = false;
  }

  void strVal(std::string_view v) {
    maybeComma();
    writeString(v);
    needsComma_ = true;
  }
  void boolVal(bool v) {
    maybeComma();
    append(v ? "true" : "false");
    needsComma_ = true;
  }
  void intVal(int64_t v) {
    maybeComma();
    append(std::to_string(v));
    needsComma_ = true;
  }
  void uintVal(uint64_t v) {
    maybeComma();
    append(std::to_string(v));
    needsComma_ = true;
  }
  void doubleVal(double v) {
    maybeComma();
    char buf[32];
    int n = std::snprintf(buf, sizeof(buf), "%g", v);
    append(std::string_view(buf, n > 0 ? static_cast<size_t>(n) : 0));
    needsComma_ = true;
  }
  void nullVal() {
    maybeComma();
    append("null");
    needsComma_ = true;
  }

  void field(std::string_view k, std::string_view v) {
    key(k);
    strVal(v);
  }
  void field(std::string_view k, bool v) {
    key(k);
    boolVal(v);
  }
  void field(std::string_view k, int64_t v) {
    key(k);
    intVal(v);
  }
  void field(std::string_view k, uint64_t v) {
    key(k);
    uintVal(v);
  }
  void field(std::string_view k, double v) {
    key(k);
    doubleVal(v);
  }

private:
  void maybeComma() {
    if (needsComma_) {
      append(',');
    }
  }

  void append(char c) { app_.write(static_cast<uint8_t>(c)); }

  void append(std::string_view s) {
    app_.push(reinterpret_cast<const uint8_t*>(s.data()), s.size());
  }

  // Pushes unescaped runs whole. Track names and namespaces almost never need
  // escaping, and this runs on the relay executor, so per-character appends are
  // the wrong default.
  void writeString(std::string_view s) {
    append('"');
    size_t runStart = 0;
    for (size_t i = 0; i < s.size(); ++i) {
      const auto c = static_cast<unsigned char>(s[i]);
      char unicode[7];
      std::string_view escape;
      switch (c) {
      case '"':
        escape = "\\\"";
        break;
      case '\\':
        escape = "\\\\";
        break;
      case '\n':
        escape = "\\n";
        break;
      case '\r':
        escape = "\\r";
        break;
      case '\t':
        escape = "\\t";
        break;
      default:
        if (c >= 0x20) {
          continue;
        }
        std::snprintf(unicode, sizeof(unicode), "\\u%04x", c);
        escape = std::string_view(unicode, 6);
      }
      if (i > runStart) {
        append(s.substr(runStart, i - runStart));
      }
      append(escape);
      runStart = i + 1;
    }
    if (runStart < s.size()) {
      append(s.substr(runStart));
    }
    append('"');
  }

  folly::io::QueueAppender& app_;
  bool needsComma_{false};
};

} // namespace openmoq::moqx::admin
