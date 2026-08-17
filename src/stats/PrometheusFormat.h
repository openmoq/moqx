/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <folly/Conv.h>
#include <folly/io/Cursor.h>
#include <folly/io/IOBuf.h>
#include <folly/io/IOBufQueue.h>

namespace openmoq::moqx::stats {

// Escapes a label value for the Prometheus text exposition format, which only
// defines \\, \" and \n.
inline std::string escapeLabelValue(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (char c : value) {
    switch (c) {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case '\n':
      out += "\\n";
      break;
    default:
      out += c;
    }
  }
  return out;
}

// Accumulates a text exposition format v0.0.4 response:
//   https://prometheus.io/docs/instrumenting/exposition_formats/
class PrometheusWriter {
public:
  PrometheusWriter() = default;
  // omitMetadata drops the # HELP / # TYPE lines, which repeat unchanged on
  // every scrape and dominate the response for small series counts.
  explicit PrometheusWriter(bool omitMetadata) : omitMetadata_(omitMetadata) {}

  void append(std::string_view s) {
    appender_.push(reinterpret_cast<const uint8_t*>(s.data()), s.size());
  }

  template <typename T> void num(T value) { append(folly::to<std::string>(value)); }

  void header(std::string_view name, std::string_view type, std::string_view help) {
    if (omitMetadata_) {
      return;
    }
    append("# HELP ");
    append(name);
    append(" ");
    append(help);
    append("\n# TYPE ");
    append(name);
    append(" ");
    append(type);
    append("\n");
  }

  std::unique_ptr<folly::IOBuf> move() { return queue_.move(); }

private:
  bool omitMetadata_{false};
  folly::IOBufQueue queue_{folly::IOBufQueue::cacheChainLength()};
  folly::io::QueueAppender appender_{&queue_, 8192};
};

} // namespace openmoq::moqx::stats
