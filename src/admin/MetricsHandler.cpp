#include <moqx/admin/MetricsHandler.h>

#include <folly/CancellationToken.h>
#include <folly/coro/Task.h>
#include <folly/experimental/coro/Task.h>
#include <folly/experimental/coro/WithCancellation.h>
#include <folly/io/IOBuf.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/logging/xlog.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/lib/http/HTTPMessage.h>

#include <moqx/admin/AdminServer.h>
#include <moqx/stats/StatsRegistry.h>

namespace openmoq::moqx::admin {

void registerMetricsRoute(
    AdminServer& adminServer,
    std::shared_ptr<stats::StatsRegistry> registry
) {
  adminServer.addRoute(
      "GET",
      "/metrics",
      [registry = std::move(registry
       )](auto /*req*/, auto /*body*/, auto* downstream, folly::CancellationToken cancelToken) {
        auto* evb = folly::EventBaseManager::get()->getEventBase();

        folly::coro::co_withCancellation(
            cancelToken,
            folly::coro::co_withExecutor(
                evb,
                [](auto reg, auto* ds, auto token) -> folly::coro::Task<void> {
                  stats::StatsSnapshot snap;
                  try {
                    snap = co_await reg->aggregateAsync();
                  } catch (const std::exception& e) {
                    XLOG(ERR) << "MetricsHandler: aggregateAsync threw: " << e.what();
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
                  auto body = stats::StatsSnapshot::formatPrometheus(snap);
                  proxygen::ResponseBuilder(ds)
                      .status(200, proxygen::HTTPMessage::getDefaultReason(200))
                      .header("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
                      .body(std::move(body))
                      .sendWithEOM();
                }(registry, downstream, cancelToken)
            )
        )
            .start();
      }
  );
}

} // namespace openmoq::moqx::admin
