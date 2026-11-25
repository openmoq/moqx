/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

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

// One template variable. render() appends the value and returns true, or returns false
// for "-".
struct AccessLogVar {
  std::string_view name;
  uint8_t classes;
  bool (*render)(const AccessLogRecord&, const AccessLogRenderContext&, std::string& out);
};

std::span<const AccessLogVar> accessLogVars();
const AccessLogVar* findAccessLogVar(std::string_view name);

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
