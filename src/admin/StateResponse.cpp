/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "admin/StateResponse.h"

#include <folly/logging/xlog.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/lib/http/HTTPMessage.h>

namespace openmoq::moqx::admin {

folly::coro::Task<void> sendState(
    proxygen::ResponseHandler* ds,
    folly::CancellationToken token,
    folly::coro::AsyncGenerator<std::unique_ptr<folly::IOBuf>&&> gen,
    std::shared_ptr<StreamBudget> budget,
    std::shared_ptr<EgressGate> egress
) {
  bool headersSent = false;

  // Headers ride the first chunk, so a failure before any output is still a
  // status rather than an aborted 200.
  auto begin = [&](proxygen::ResponseBuilder& builder) {
    if (!headersSent) {
      builder.status(200, proxygen::HTTPMessage::getDefaultReason(200))
          .header("Content-Type", "application/json");
      headersSent = true;
    }
  };

  auto sendChunk = [&](std::unique_ptr<folly::IOBuf> chunk) {
    budget->release(chunk->computeChainDataLength());
    proxygen::ResponseBuilder builder(ds);
    begin(builder);
    builder.body(std::move(chunk)).send();
  };

  auto finish = [&] {
    proxygen::ResponseBuilder builder(ds);
    begin(builder);
    builder.sendWithEOM();
  };

  // Ends a response that cannot be finished normally. Only safe with the token
  // clear: onError sets it before deleting the handler that owns ds.
  auto fail = [&] {
    if (headersSent) {
      // Already committed to 200; a truncated body is the only signal left.
      ds->sendAbort();
    } else {
      proxygen::ResponseBuilder(ds)
          .status(500, proxygen::HTTPMessage::getDefaultReason(500))
          .body(folly::IOBuf::copyBuffer("internal error\n"))
          .sendWithEOM();
    }
  };

  try {
    while (true) {
      // Not pulling while proxygen is not draining is what makes the cap bound
      // real memory rather than relocate it into the egress queue. The common
      // case never parks, so it never pays for a coroutine frame either.
      if (egress->paused() && !co_await egress->awaitDrainable()) {
        // The gate gives up on cancellation, which the token check covers, and
        // on egress timing out, which does not: leaving that response
        // unterminated would hang the client until proxygen gave up too.
        if (!token.isCancellationRequested()) {
          fail();
        }
        co_return;
      }
      if (token.isCancellationRequested()) {
        co_return;
      }
      auto chunk = co_await gen.next();
      if (!chunk) {
        break;
      }
      if (token.isCancellationRequested()) {
        co_return;
      }
      sendChunk(std::move(*chunk));
    }
  } catch (const std::exception& e) {
    XLOG(ERR) << "/state: walk threw: " << e.what();
    if (!token.isCancellationRequested()) {
      fail();
    }
    co_return;
  }

  if (token.isCancellationRequested()) {
    co_return;
  }
  finish();
}

} // namespace openmoq::moqx::admin
