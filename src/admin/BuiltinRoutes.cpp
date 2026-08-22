/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "admin/BuiltinRoutes.h"

#include <chrono>

#include <folly/Conv.h>
#include <folly/io/IOBuf.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/lib/http/HTTPMessage.h>

#include "admin/AdminServer.h"
#include "moqx/Version.h"

namespace openmoq::moqx::admin {

void registerBuiltinRoutes(AdminServer& server) {
  // Registration happens once during startup, so this is the process start for
  // dashboard purposes. steady_clock drives uptime so wall-clock jumps (NTP)
  // can't make it go backwards.
  const auto startWall = std::chrono::system_clock::now();
  const auto startSteady = std::chrono::steady_clock::now();
  const auto startEpoch =
      std::chrono::duration_cast<std::chrono::seconds>(startWall.time_since_epoch()).count();

  server.addRoute(
      "GET",
      "/info",
      [startEpoch, startSteady](
          auto /*req*/,
          auto /*body*/,
          auto* downstream,
          auto /*cancelToken*/,
          const auto& /*egress*/
      ) {
        const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::steady_clock::now() - startSteady
        )
                                .count();
        auto body = folly::to<std::string>(
            "{\"service\":\"moqx\",\"version\":\"" MOQX_VERSION "\",\"start_time\":",
            startEpoch,
            ",\"uptime_seconds\":",
            uptime,
            "}\n"
        );
        proxygen::ResponseBuilder(downstream)
            .status(200, proxygen::HTTPMessage::getDefaultReason(200))
            .header("Content-Type", "application/json")
            .body(folly::IOBuf::copyBuffer(body))
            .sendWithEOM();
      }
  );
}

} // namespace openmoq::moqx::admin
