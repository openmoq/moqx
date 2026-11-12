/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "logging/access/AccessLogTemplate.h"

#include <algorithm>

#include <fmt/format.h>

namespace openmoq::moqx::logging {

namespace {

// Names are lowercase. Uppercase letters still end up in the name, so "$Event" fails as
// an unknown variable.
constexpr std::string_view kNameChars =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_";

} // namespace

folly::Expected<AccessLogTemplate, std::string> AccessLogTemplate::parse(std::string_view text) {
  AccessLogTemplate tmpl;
  tmpl.text_ = std::string(text);
  std::string literal;
  bool hasEvent = false;
  size_t i = 0;
  while (true) {
    auto dollar = text.find('$', i);
    auto run = text.substr(i, dollar - i);
    if (auto nl = run.find_first_of("\r\n"); nl != std::string_view::npos) {
      return folly::makeUnexpected(fmt::format("line break at offset {}", i + nl));
    }
    literal.append(run);
    if (dollar == std::string_view::npos) {
      break;
    }
    std::string_view name;
    if (text.substr(dollar + 1).starts_with('{')) {
      auto close = text.find('}', dollar + 2);
      if (close == std::string_view::npos) {
        return folly::makeUnexpected(fmt::format("unterminated ${{ at offset {}", dollar));
      }
      name = text.substr(dollar + 2, close - dollar - 2);
      i = close + 1;
    } else {
      i = std::min(text.find_first_not_of(kNameChars, dollar + 1), text.size());
      name = text.substr(dollar + 1, i - dollar - 1);
    }
    if (name.empty()) {
      return folly::makeUnexpected(fmt::format("'$' without a variable name at offset {}", dollar));
    }
    const auto* var = findAccessLogVar(name);
    if (!var) {
      return folly::makeUnexpected(fmt::format("unknown variable ${}", name));
    }
    if (!hasEvent && var->classes != kAllAccessLogClasses) {
      return folly::makeUnexpected(fmt::format("${} must come after $event", name));
    }
    if (name == "event") {
      hasEvent = true;
      tmpl.eventSegment_ = tmpl.segments_.size();
      tmpl.eventEnd_ = i;
    }
    tmpl.segments_.push_back({std::move(literal), var});
    literal.clear();
  }
  if (!literal.empty()) {
    tmpl.segments_.push_back({std::move(literal), nullptr});
  }
  if (!hasEvent) {
    return folly::makeUnexpected(std::string("template must include $event"));
  }
  return tmpl;
}

void AccessLogTemplate::render(
    const AccessLogRecord& record,
    const AccessLogRenderContext& ctx,
    std::string& out
) const {
  for (const auto& seg : segments_) {
    out.append(seg.literal);
    if (seg.var) {
      renderAccessLogVar(*seg.var, record, ctx, out);
    }
  }
}

bool AccessLogTemplate::sharesPrefixWith(const AccessLogTemplate& other) const {
  return eventSegment_ == other.eventSegment_ && std::equal(
                                                     segments_.begin(),
                                                     segments_.begin() + eventSegment_ + 1,
                                                     other.segments_.begin()
                                                 );
}

} // namespace openmoq::moqx::logging
