#include <o_rly/admin/MetricsHandler.h>

#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Task.h>
#include <folly/io/IOBuf.h>
#include <folly/logging/xlog.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/lib/http/HTTPMessage.h>

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
      [registry = std::move(registry)](auto /*req*/, auto /*body*/, auto* downstream) {
        try {
          const auto& snapshot = folly::coro::blockingWait(registry->aggregateAsync());
          auto body = stats::StatsSnapshot::formatPrometheus(snapshot);

          proxygen::ResponseBuilder(downstream)
              .status(200, proxygen::HTTPMessage::getDefaultReason(200))
              // Prometheus text exposition format v0.0.4
              .header("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
              .body(std::move(body))
              .sendWithEOM();
        } catch (const std::exception& ex) {
          XLOG(ERR) << "MetricsHandler: aggregateAsync threw: " << ex.what();
          proxygen::ResponseBuilder(downstream)
              .status(500, proxygen::HTTPMessage::getDefaultReason(500))
              .body(folly::IOBuf::copyBuffer("internal error\n"))
              .sendWithEOM();
        }
      }
  );
}

} // namespace openmoq::o_rly::admin
