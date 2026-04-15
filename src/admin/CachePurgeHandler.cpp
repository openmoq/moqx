/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "admin/CachePurgeHandler.h"

#include <folly/CancellationToken.h>
#include <folly/coro/Task.h>
#include <folly/experimental/coro/WithCancellation.h>
#include <folly/io/IOBuf.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/json/json.h>
#include <folly/logging/xlog.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/lib/http/HTTPMessage.h>

#include "MoqxRelayContext.h"
#include "admin/AdminServer.h"

namespace openmoq::moqx::admin {

void registerCachePurgeRoute(AdminServer& adminServer, std::shared_ptr<MoqxRelayContext> context) {
  adminServer.addRoute(
      "POST",
      "/cache/purge",
      [weak = std::weak_ptr<MoqxRelayContext>(context
       )](std::unique_ptr<proxygen::HTTPMessage> /*req*/,
          std::unique_ptr<folly::IOBuf> body,
          proxygen::ResponseHandler* downstream,
          folly::CancellationToken cancelToken) {
        auto ctx = weak.lock();
        if (!ctx) {
          proxygen::ResponseBuilder(downstream)
              .status(503, proxygen::HTTPMessage::getDefaultReason(503))
              .body(folly::IOBuf::copyBuffer("{\"error\":\"server unavailable\"}\n"))
              .sendWithEOM();
          return;
        }

        // Parse optional JSON body: {"service":"...","namespace":"...","track":"..."}
        // Done synchronously before launching the coroutine.
        std::string serviceName;
        std::optional<moxygen::FullTrackName> ftn;
        std::optional<moxygen::TrackNamespace> nsOnly;
        if (body) {
          try {
            auto bodyStr = body->moveToFbString().toStdString();
            if (!bodyStr.empty()) {
              auto parsed = folly::parseJson(bodyStr);
              if (parsed.isObject()) {
                if (auto* svc = parsed.get_ptr("service")) {
                  serviceName = svc->asString();
                }
                if (auto* ns = parsed.get_ptr("namespace")) {
                  if (auto* trk = parsed.get_ptr("track")) {
                    ftn = moxygen::FullTrackName{
                        moxygen::TrackNamespace(ns->asString(), "/"),
                        trk->asString()
                    };
                  } else {
                    nsOnly = moxygen::TrackNamespace(ns->asString(), "/");
                  }
                }
              }
            }
          } catch (const std::exception& e) {
            proxygen::ResponseBuilder(downstream)
                .status(400, proxygen::HTTPMessage::getDefaultReason(400))
                .header("Content-Type", "application/json")
                .body(folly::IOBuf::copyBuffer("{\"error\":\"invalid JSON body\"}\n"))
                .sendWithEOM();
            return;
          }
        }

        auto* cacheEvb = ctx->cacheEvb();
        if (!cacheEvb) {
          proxygen::ResponseBuilder(downstream)
              .status(503, proxygen::HTTPMessage::getDefaultReason(503))
              .body(folly::IOBuf::copyBuffer("{\"error\":\"cache not ready\"}\n"))
              .sendWithEOM();
          return;
        }

        // Outer coroutine stays on the admin EVB so sendWithEOM is thread-safe.
        // The inner co_withExecutor(cacheEvb, ...) switches to the cache EVB to
        // run the eviction, then resumes here on the admin EVB.
        folly::coro::co_withCancellation(
            cancelToken,
            folly::coro::co_withExecutor(
                folly::EventBaseManager::get()->getEventBase(),
                [](auto c,
                   auto svcName,
                   auto maybeFtn,
                   auto maybeNs,
                   auto* ds,
                   auto* cEvb,
                   auto token) -> folly::coro::Task<void> {
                  size_t evicted = 0;
                  try {
                    evicted = co_await folly::coro::co_withExecutor(
                        cEvb,
                        c->purgeCache(svcName, maybeFtn, maybeNs)
                    );
                  } catch (const std::exception& e) {
                    XLOG(ERR) << "CachePurgeHandler: purge threw: " << e.what();
                    if (!token.isCancellationRequested()) {
                      proxygen::ResponseBuilder(ds)
                          .status(500, proxygen::HTTPMessage::getDefaultReason(500))
                          .body(folly::IOBuf::copyBuffer("{\"error\":\"internal error\"}\n"))
                          .sendWithEOM();
                    }
                    co_return;
                  }
                  if (token.isCancellationRequested()) {
                    co_return;
                  }
                  auto respBody = std::string("{\"evicted\":") + std::to_string(evicted) + "}\n";
                  proxygen::ResponseBuilder(ds)
                      .status(200, proxygen::HTTPMessage::getDefaultReason(200))
                      .header("Content-Type", "application/json")
                      .body(folly::IOBuf::copyBuffer(respBody))
                      .sendWithEOM();
                }(ctx, serviceName, ftn, nsOnly, downstream, cacheEvb, cancelToken)
            )
        )
            .start();
      }
  );
}

} // namespace openmoq::moqx::admin
