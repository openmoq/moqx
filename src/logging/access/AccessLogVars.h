/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include "logging/access/AccessLogRecord.h"

namespace openmoq::moqx::logging {

inline constexpr std::string_view kAccessLogSchema = "moqx.v1";

// Per-sink values supplied at render time.
struct AccessLogRenderContext {
  const AccessLogRelayInfo& relayInfo;
  uint64_t seq{0};
};

// The bit for each class in AccessLogVar::classes.
constexpr uint8_t accessLogClassBit(AccessLogClass cls) {
  return uint8_t(1u << uint8_t(cls));
}
inline constexpr uint8_t kAllAccessLogClasses = 0x0f;

// A variable's value, or monostate where it does not apply or is not known. Text is not
// escaped. A string_view points into the record or the render context.
using AccessLogValue = std::variant<
    std::monostate,
    uint64_t,
    bool,
    std::string_view,
    std::string,
    moxygen::AbsoluteLocation,
    std::chrono::system_clock::time_point>;

// How a line renders a variable's text or time.
enum class AccessLogTextForm : uint8_t {
  Plain,
  // Free text, passed through appendEscaped().
  Escaped,
  // A safeName() form, where an empty name prints as "".
  Name,
  IsoTime,
  EpochMsec,
};

// One template variable.
struct AccessLogVar {
  std::string_view name;
  uint8_t classes;
  AccessLogTextForm form;
  AccessLogValue (*value)(const AccessLogRecord&, const AccessLogRenderContext&);
};

std::span<const AccessLogVar> accessLogVars();
const AccessLogVar* findAccessLogVar(std::string_view name);

// One variable's value, or monostate if the variable does not apply to the record's class.
AccessLogValue accessLogVarValue(
    const AccessLogVar& var,
    const AccessLogRecord& record,
    const AccessLogRenderContext& ctx
);

// Appends one variable's value or "-".
void renderAccessLogVar(
    const AccessLogVar& var,
    const AccessLogRecord& record,
    const AccessLogRenderContext& ctx,
    std::string& out
);

// Escapes space, '"', '\' and non-printable bytes as \xHH, so a free-text value stays one
// positional column.
void appendEscaped(std::string_view text, std::string& out);

} // namespace openmoq::moqx::logging
