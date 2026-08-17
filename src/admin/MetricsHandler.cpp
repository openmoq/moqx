/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "admin/MetricsHandler.h"

#include <folly/CancellationToken.h>
#include <folly/coro/Task.h>
#include <folly/coro/WithCancellation.h>
#include <folly/io/IOBuf.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/logging/xlog.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/lib/http/HTTPMessage.h>

#include "admin/AdminResponse.h"
#include "admin/AdminServer.h"
#include "stats/StatsRegistry.h"

namespace openmoq::moqx::admin {

void registerMetricsRoute(
    AdminServer& adminServer,
    std::shared_ptr<stats::StatsRegistry> registry
) {
  adminServer.addRoute(
      "GET",
      "/metrics",
      [registry = std::move(registry
       )](auto req, auto /*body*/, auto* downstream, folly::CancellationToken cancelToken) {
        auto* evb = folly::EventBaseManager::get()->getEventBase();

        auto omitMetadata = boolQueryParam(*req, "omit_metadata", false);
        if (!omitMetadata) {
          sendError(downstream, 400, "omit_metadata must be one of 1, 0, true, false\n");
          return;
        }

        folly::coro::co_withCancellation(
            cancelToken,
            folly::coro::co_withExecutor(
                evb,
                [](auto reg, auto* ds, auto token, bool omitMeta) -> folly::coro::Task<void> {
                  stats::StatsSnapshot snap;
                  try {
                    snap = co_await reg->aggregateAsync();
                  } catch (const std::exception& e) {
                    XLOG(ERR) << "MetricsHandler: aggregateAsync threw: " << e.what();
                    if (!token.isCancellationRequested()) {
                      sendError(ds, 500, "internal error\n");
                    }
                    co_return;
                  }
                  if (token.isCancellationRequested()) {
                    co_return;
                  }
                  auto body = stats::StatsSnapshot::formatPrometheus(snap, omitMeta);
                  proxygen::ResponseBuilder(ds)
                      .status(200, proxygen::HTTPMessage::getDefaultReason(200))
                      .header("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
                      .body(std::move(body))
                      .sendWithEOM();
                }(registry, downstream, cancelToken, *omitMetadata)
            )
        )
            .start();
      }
  );
}

} // namespace openmoq::moqx::admin
