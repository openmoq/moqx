/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "logging/access/TemplateEncoder.h"

#include <fmt/format.h>
#include <glog/logging.h>

namespace openmoq::moqx::logging {

namespace {

AccessLogTemplate mustParse(std::string_view text) {
  auto tmpl = AccessLogTemplate::parse(text);
  CHECK(tmpl.hasValue()) << tmpl.error();
  return std::move(*tmpl);
}

} // namespace

void TemplateEncoder::setClassTemplate(AccessLogClass cls, AccessLogTemplate tmpl) {
  classTemplates_[size_t(cls)] = std::move(tmpl);
}

void TemplateEncoder::setEventTemplate(AccessLogEvent event, AccessLogTemplate tmpl) {
  eventTemplates_[size_t(event)] = std::move(tmpl);
}

void TemplateEncoder::encode(
    const AccessLogRecord& record,
    const AccessLogRenderContext& ctx,
    std::string& out
) const {
  if (const auto& tmpl = eventTemplates_[size_t(record.event)]) {
    tmpl->render(record, ctx, out);
  } else if (const auto& cls = classTemplates_[size_t(accessLogClass(record.event))]) {
    cls->render(record, ctx, out);
  } else {
    out.append(accessLogEventName(record.event));
  }
}

std::optional<std::string> TemplateEncoder::alignPrefixes() {
  const AccessLogTemplate* first = nullptr;
  auto check = [&](const std::optional<AccessLogTemplate>& tmpl) -> std::optional<std::string> {
    if (!tmpl) {
      return std::nullopt;
    }
    if (!first) {
      first = &*tmpl;
    } else if (!tmpl->sharesPrefixWith(*first)) {
      return fmt::format(
          "'{}' and '{}' must match up to and including $event",
          first->text(),
          tmpl->text()
      );
    }
    return std::nullopt;
  };
  for (size_t cls = 0; cls < classTemplates_.size(); ++cls) {
    if (AccessLogClass(cls) == AccessLogClass::Internal) {
      continue;
    }
    if (auto error = check(classTemplates_[cls])) {
      return error;
    }
  }
  for (const auto& tmpl : eventTemplates_) {
    if (auto error = check(tmpl)) {
      return error;
    }
  }
  if (first) {
    auto prefix = std::string(first->prefixText());
    classTemplates_[size_t(AccessLogClass::Internal)] = mustParse(prefix + " $dropped");
    for (auto& tmpl : classTemplates_) {
      if (!tmpl) {
        tmpl = mustParse(prefix);
      }
    }
  }
  return std::nullopt;
}

TemplateEncoder defaultTemplateEncoder() {
  TemplateEncoder encoder;
  encoder.setClassTemplate(
      AccessLogClass::Session,
      mustParse("$schema $time $event $session $opened $peer_trunc $peer_class $service $version "
                "$status $latency_ms $error_class $duration_ms")
  );
  encoder.setClassTemplate(
      AccessLogClass::Request,
      mustParse("$schema $time $event $session $dir $req $ftn $status $latency_ms $error_class")
  );
  encoder.setClassTemplate(AccessLogClass::Internal, mustParse("$schema $time $event $dropped"));
  return encoder;
}

folly::Expected<TemplateEncoder, std::string> makeTemplateEncoder(
    const std::map<std::string, std::string>& classFormats,
    const std::map<std::string, std::string>& eventFormats
) {
  auto encoder = defaultTemplateEncoder();
  for (const auto& [name, text] : classFormats) {
    auto cls = parseAccessLogClass(name);
    if (!cls || *cls == AccessLogClass::Internal) {
      return folly::makeUnexpected(fmt::format("unknown class '{}'", name));
    }
    auto tmpl = AccessLogTemplate::parse(text);
    if (tmpl.hasError()) {
      return folly::makeUnexpected(fmt::format("format.{}: {}", name, tmpl.error()));
    }
    encoder.setClassTemplate(*cls, std::move(*tmpl));
  }
  for (const auto& [name, text] : eventFormats) {
    auto event = parseAccessLogEvent(name);
    if (!event || accessLogClass(*event) == AccessLogClass::Internal) {
      return folly::makeUnexpected(fmt::format("unknown event '{}'", name));
    }
    auto tmpl = AccessLogTemplate::parse(text);
    if (tmpl.hasError()) {
      return folly::makeUnexpected(fmt::format("format.event.{}: {}", name, tmpl.error()));
    }
    encoder.setEventTemplate(*event, std::move(*tmpl));
  }
  if (auto error = encoder.alignPrefixes()) {
    return folly::makeUnexpected(std::move(*error));
  }
  return encoder;
}

} // namespace openmoq::moqx::logging
