/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "admin/StateHandler.h"

#include <folly/CancellationToken.h>
#include <folly/coro/Task.h>
#include <folly/coro/WithCancellation.h>
#include <folly/io/IOBuf.h>
#include <folly/io/IOBufQueue.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/logging/xlog.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/lib/http/HTTPMessage.h>

#include "MoqxRelayContext.h"
#include "admin/AdminServer.h"
#include "admin/ChunkedJsonWriter.h"
#include "admin/JsonStateVisitors.h"

namespace openmoq::moqx::admin {

namespace {

// Awaitable from the admin EVB: dumpState hops to each service's own executor
// and back for itself.
folly::coro::Task<std::unique_ptr<folly::IOBuf>>
buildStateBody(std::shared_ptr<MoqxRelayContext> ctx) {
  folly::IOBufQueue body{folly::IOBufQueue::cacheChainLength()};
  {
    // Chunks are collected rather than sent: the response still goes out whole
    // at EOM.
    ChunkedJsonWriter writer([&body](std::unique_ptr<folly::IOBuf> chunk) {
      body.append(std::move(chunk));
      return true;
    });
    JsonRelayContextVisitor visitor(writer);
    co_await ctx->dumpState(visitor);
    writer.flush();
  }
  co_return body.move();
}

} // namespace

void registerStateRoute(AdminServer& adminServer, std::shared_ptr<MoqxRelayContext> context) {
  adminServer.addRoute(
      "GET",
      "/state",
      [context = std::move(context
       )](auto /*req*/,
          auto /*body*/,
          auto* downstream,
          folly::CancellationToken cancelToken,
          const std::shared_ptr<EgressGate>& /*egress*/) {
        if (!context->ready()) {
          proxygen::ResponseBuilder(downstream)
              .status(503, proxygen::HTTPMessage::getDefaultReason(503))
              .body(folly::IOBuf::copyBuffer("relay not ready\n"))
              .sendWithEOM();
          return;
        }

        // The coroutine is pinned to the admin EVB so sendWithEOM is
        // thread-safe; the walk's own executor hops happen inside the co_await
        // and it resumes here.
        folly::coro::co_withCancellation(
            cancelToken,
            folly::coro::co_withExecutor(
                folly::EventBaseManager::get()->getEventBase(),
                [](auto ctx, auto* ds, auto token) -> folly::coro::Task<void> {
                  if (token.isCancellationRequested()) {
                    co_return;
                  }
                  std::unique_ptr<folly::IOBuf> body;
                  try {
                    body = co_await buildStateBody(ctx);
                  } catch (const std::exception& e) {
                    XLOG(ERR) << "StateHandler: dumpState threw: " << e.what();
                    if (!token.isCancellationRequested()) {
                      proxygen::ResponseBuilder(ds)
                          .status(500, proxygen::HTTPMessage::getDefaultReason(500))
                          .body(folly::IOBuf::copyBuffer("internal error\n"))
                          .sendWithEOM();
                    }
                    co_return;
                  }
                  if (token.isCancellationRequested()) {
                    co_return;
                  }
                  proxygen::ResponseBuilder(ds)
                      .status(200, proxygen::HTTPMessage::getDefaultReason(200))
                      .header("Content-Type", "application/json")
                      .body(std::move(body))
                      .sendWithEOM();
                }(context, downstream, cancelToken)
            )
        )
            .start();
      }
  );
}

} // namespace openmoq::moqx::admin
