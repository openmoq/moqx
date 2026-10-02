/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <folly/Conv.h>
#include <folly/io/IOBuf.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/httpserver/ResponseHandler.h>
#include <proxygen/lib/http/HTTPMessage.h>

namespace openmoq::moqx::admin {

inline void
sendError(proxygen::ResponseHandler* downstream, int status, const std::string& message) {
  proxygen::ResponseBuilder(downstream)
      .status(status, proxygen::HTTPMessage::getDefaultReason(status))
      .header("Content-Type", "text/plain; charset=utf-8")
      .body(folly::IOBuf::copyBuffer(message))
      .sendWithEOM();
}

// Returns nullopt if the param is present with a value that isn't a boolean.
// A bare `?flag` with no value reads as true, as flags conventionally do.
inline std::optional<bool>
boolQueryParam(const proxygen::HTTPMessage& req, const std::string& name, bool defaultValue) {
  if (!req.hasQueryParam(name)) {
    return defaultValue;
  }
  auto value = req.getDecodedQueryParam(name);
  if (value.empty() || value == "1" || value == "true") {
    return true;
  }
  if (value == "0" || value == "false") {
    return false;
  }
  return std::nullopt;
}

// Returns nullopt if the param is present but not an integer within [1, max].
inline std::optional<uint32_t> boundedQueryParam(
    const proxygen::HTTPMessage& req,
    const std::string& name,
    uint32_t defaultValue,
    uint32_t max
) {
  if (!req.hasQueryParam(name)) {
    return defaultValue;
  }
  auto value = folly::tryTo<uint32_t>(req.getDecodedQueryParam(name));
  if (!value || *value < 1 || *value > max) {
    return std::nullopt;
  }
  return *value;
}

} // namespace openmoq::moqx::admin
