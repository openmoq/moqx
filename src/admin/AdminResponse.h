/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>

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

} // namespace openmoq::moqx::admin
