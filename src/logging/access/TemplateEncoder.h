/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>

#include <folly/Expected.h>

#include "logging/access/AccessLogTemplate.h"

namespace openmoq::moqx::logging {

// Renders the event's template if it has one, else its class's template.
class TemplateEncoder {
public:
  void setClassTemplate(AccessLogClass cls, AccessLogTemplate tmpl);
  void setEventTemplate(AccessLogEvent event, AccessLogTemplate tmpl);

  // Appends one line, without its newline.
  void
  encode(const AccessLogRecord& record, const AccessLogRenderContext& ctx, std::string& out) const;

  // Fails if two templates differ up to and including $event. Otherwise gives that
  // prefix to the LOG_DROPPED line and to each class without a template.
  std::optional<std::string> alignPrefixes();

private:
  std::array<std::optional<AccessLogTemplate>, 4> classTemplates_;
  std::array<std::optional<AccessLogTemplate>, size_t(AccessLogEvent::LOG_DROPPED) + 1>
      eventTemplates_;
};

// The default templates for the session, request and internal classes. Each starts with
// "$schema $time $event".
TemplateEncoder defaultTemplateEncoder();

// The default templates with a sink's overrides, keyed by class or event name. Fails on an
// unknown name, an invalid template, or templates whose prefixes differ.
folly::Expected<TemplateEncoder, std::string> makeTemplateEncoder(
    const std::map<std::string, std::string>& classFormats,
    const std::map<std::string, std::string>& eventFormats
);

} // namespace openmoq::moqx::logging
