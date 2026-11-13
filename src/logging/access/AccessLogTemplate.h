/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <folly/Expected.h>

#include "logging/access/AccessLogVars.h"

namespace openmoq::moqx::logging {

// Operator-defined variables, such as region or pop. A template prints each one as
// literal text.
using AccessLogStaticVars = std::map<std::string, std::string>;

// An nginx-style line template: literal text with $name or ${name} variables. Parsed
// once at config time, rendered on the writer thread.
class AccessLogTemplate {
public:
  // Fails on an unknown variable, a dangling '$', a line break, a template without
  // $event, or a class-specific variable before $event.
  static folly::Expected<AccessLogTemplate, std::string>
  parse(std::string_view text, const AccessLogStaticVars& staticVars = {});

  void
  render(const AccessLogRecord& record, const AccessLogRenderContext& ctx, std::string& out) const;

  // True if both templates render the same text up to and including $event, so a reader
  // finds $event at the same column on every line of a sink.
  bool sharesPrefixWith(const AccessLogTemplate& other) const;
  // The template text up to and including $event.
  std::string_view prefixText() const { return std::string_view(text_).substr(0, eventEnd_); }

  const std::string& text() const { return text_; }

private:
  struct Segment {
    std::string literal;
    const AccessLogVar* var{nullptr};

    bool operator==(const Segment&) const = default;
  };

  std::string text_;
  std::vector<Segment> segments_;
  size_t eventSegment_{0};
  size_t eventEnd_{0};
};

} // namespace openmoq::moqx::logging
