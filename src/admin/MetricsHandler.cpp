#include <o_rly/admin/MetricsHandler.h>

#include <folly/CancellationToken.h>
#include <folly/Try.h>
#include <folly/coro/Task.h>
#include <folly/coro/WithCancellation.h>
#include <folly/io/IOBuf.h>
#include <folly/logging/xlog.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/lib/http/HTTPMessage.h>
#include <proxygen/lib/http/session/HTTPSessionBase.h>
#include <proxygen/lib/http/session/HTTPTransaction.h>

#include <o_rly/admin/AdminServer.h>
#include <o_rly/stats/StatsRegistry.h>

namespace openmoq::o_rly::admin {

void registerMetricsRoute(
    AdminServer& adminServer,
    std::shared_ptr<stats::StatsRegistry> registry
) {
  adminServer.addRoute(
      "GET",
      "/metrics",
      [registry = std::move(registry
       )](auto /*req*/, auto /*body*/, auto* downstream, folly::CancellationToken cancelToken) {
        // Use the proxygen transaction's EventBase as the coroutine executor.
        auto* evb =
            downstream->getTransaction()->getTransport().getHTTPSessionBase()->getEventBase();
        folly::coro::co_withCancellation(
            cancelToken,
            folly::coro::co_withExecutor(folly::getKeepAliveToken(evb), registry->aggregateAsync())
        )
            .start([downstream, cancelToken](folly::Try<stats::StatsSnapshot> result) noexcept {
              if (result.hasException()) {
                if (cancelToken.isCancellationRequested()) {
                  // Client disconnected before the response was produced —
                  // downstream is no longer valid; do nothing.
                  return;
                }
                XLOG(ERR) << "MetricsHandler: aggregateAsync threw: " << result.exception().what();
                proxygen::ResponseBuilder(downstream)
                    .status(500, proxygen::HTTPMessage::getDefaultReason(500))
                    .body(folly::IOBuf::copyBuffer("internal error\n"))
                    .sendWithEOM();
                return;
              }

              auto body = stats::StatsSnapshot::formatPrometheus(result.value());
              proxygen::ResponseBuilder(downstream)
                  .status(200, proxygen::HTTPMessage::getDefaultReason(200))
                  // Prometheus text exposition format v0.0.4
                  .header("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
                  .body(std::move(body))
                  .sendWithEOM();
            });
      }
  );
}

} // namespace openmoq::o_rly::admin
