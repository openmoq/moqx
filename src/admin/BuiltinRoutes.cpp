#include "admin/BuiltinRoutes.h"

#include <folly/io/IOBuf.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/lib/http/HTTPMessage.h>

#include "admin/AdminServer.h"

namespace openmoq::moqx::admin {

void registerBuiltinRoutes(AdminServer& server) {
  server.addRoute(
      "GET",
      "/info",
      [](auto /*req*/, auto /*body*/, auto* downstream, auto /*cancelToken*/) {
        proxygen::ResponseBuilder(downstream)
            .status(200, proxygen::HTTPMessage::getDefaultReason(200))
            .header("Content-Type", "application/json")
            .body(folly::IOBuf::copyBuffer("{\"service\":\"moqx\",\"version\":\"" MOQX_VERSION
                                           "\"}\n"))
            .sendWithEOM();
      }
  );
}

} // namespace openmoq::moqx::admin
