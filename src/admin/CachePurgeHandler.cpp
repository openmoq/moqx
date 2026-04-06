#include <moqx/admin/CachePurgeHandler.h>

#include <folly/io/IOBuf.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/lib/http/HTTPMessage.h>

#include <moqx/MoqxRelayServer.h>
#include <moqx/admin/AdminServer.h>

namespace openmoq::moqx::admin {

void registerCachePurgeRoute(AdminServer& server, std::shared_ptr<MoqxRelayServer> relayServer) {
  server.addRoute(
      "POST",
      "/cache-purge",
      [weak = std::weak_ptr<MoqxRelayServer>(relayServer
       )](auto* req, auto /*body*/, auto* downstream, auto /*cancelToken*/) {
        auto relay = weak.lock();
        if (!relay) {
          proxygen::ResponseBuilder(downstream)
              .status(503, proxygen::HTTPMessage::getDefaultReason(503))
              .body(folly::IOBuf::copyBuffer("{\"error\":\"server unavailable\"}\n"))
              .sendWithEOM();
          return;
        }

        std::string serviceName;
        if (req) {
          serviceName = req->getQueryParam("service");
        }

        size_t cleared = relay->clearCaches(serviceName);

        if (!serviceName.empty() && cleared == 0) {
          proxygen::ResponseBuilder(downstream)
              .status(404, proxygen::HTTPMessage::getDefaultReason(404))
              .header("Content-Type", "application/json")
              .body(folly::IOBuf::copyBuffer("{\"error\":\"service not found\"}\n"))
              .sendWithEOM();
          return;
        }

        auto body =
            std::string("{\"status\":\"ok\",\"cleared\":") + std::to_string(cleared) + "}\n";
        proxygen::ResponseBuilder(downstream)
            .status(200, proxygen::HTTPMessage::getDefaultReason(200))
            .header("Content-Type", "application/json")
            .body(folly::IOBuf::copyBuffer(body))
            .sendWithEOM();
      }
  );
}

} // namespace openmoq::moqx::admin
