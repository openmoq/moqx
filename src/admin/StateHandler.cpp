/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "admin/StateHandler.h"

#include <folly/CancellationToken.h>
#include <folly/coro/AsyncPipe.h>
#include <folly/coro/Task.h>
#include <folly/coro/WithCancellation.h>
#include <folly/io/IOBuf.h>
#include <folly/io/async/EventBaseManager.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/lib/http/HTTPMessage.h>

#include "MoqxRelayContext.h"
#include "admin/AdminServer.h"
#include "admin/ChunkedJsonWriter.h"
#include "admin/EgressGate.h"
#include "admin/JsonStateVisitors.h"
#include "admin/StateResponse.h"

namespace openmoq::moqx::admin {

namespace {

using ChunkPipe = folly::coro::AsyncPipe<std::unique_ptr<folly::IOBuf>>;

// Runs the walk, hopping to each service's owning executor, and pushes chunks
// into the pipe as they are produced. Never blocks on the consumer.
folly::coro::Task<void> produceState(
    std::shared_ptr<MoqxRelayContext> ctx,
    ChunkPipe pipe,
    std::shared_ptr<StreamBudget> budget
) {
  folly::exception_wrapper failure;
  {
    ChunkedJsonWriter writer([&](std::unique_ptr<folly::IOBuf> chunk) {
      if (!budget->tryAdd(chunk->computeChainDataLength())) {
        failure = folly::make_exception_wrapper<std::runtime_error>(
            "/state exceeded the buffered-response cap"
        );
        return false;
      }
      return pipe.write(std::move(chunk));
    });
    JsonRelayContextVisitor visitor(writer);
    auto result = co_await folly::coro::co_awaitTry(ctx->dumpState(visitor));
    if (result.hasException()) {
      failure = std::move(result).exception();
    } else {
      writer.flush();
    }
  }
  if (failure) {
    std::move(pipe).close(std::move(failure));
  } else {
    std::move(pipe).close();
  }
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
          std::shared_ptr<EgressGate> egress) {
        if (!context->ready()) {
          proxygen::ResponseBuilder(downstream)
              .status(503, proxygen::HTTPMessage::getDefaultReason(503))
              .body(folly::IOBuf::copyBuffer("relay not ready\n"))
              .sendWithEOM();
          return;
        }

        auto* adminEvb = folly::EventBaseManager::get()->getEventBase();
        auto budget = std::make_shared<StreamBudget>();
        auto [gen, pipe] = ChunkPipe::create();

        // Producer and consumer run concurrently: the producer migrates across
        // relay executors while the consumer stays on the admin EVB, where
        // sendBody is thread-safe. It re-checks the token before every send,
        // which is safe because onError sets it before deleting the handler on
        // this same single-threaded EVB.
        folly::coro::co_withExecutor(adminEvb, produceState(context, std::move(pipe), budget))
            .start();

        folly::coro::co_withCancellation(
            cancelToken,
            folly::coro::co_withExecutor(
                adminEvb,
                sendState(downstream, cancelToken, std::move(gen), budget, std::move(egress))
            )
        )
            .start();
      }
  );
}

} // namespace openmoq::moqx::admin
