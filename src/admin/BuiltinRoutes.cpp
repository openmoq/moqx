#include <o_rly/admin/BuiltinRoutes.h>

#include <folly/io/IOBuf.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/lib/http/HTTPMessage.h>

#include <o_rly/admin/AdminServer.h>

namespace openmoq::o_rly::admin {

void registerBuiltinRoutes(AdminServer& server) {
  server.addRoute(
      "GET",
      "/info",
      [](auto /*req*/, auto /*body*/, auto* downstream, auto /*cancelToken*/) {
        proxygen::ResponseBuilder(downstream)
            .status(200, proxygen::HTTPMessage::getDefaultReason(200))
            .header("Content-Type", "application/json")
            .body(folly::IOBuf::copyBuffer("{\"service\":\"o-rly\",\"version\":\"" ORLY_VERSION
                                           "\"}\n"))
            .sendWithEOM();
      }
  );
}

} // namespace openmoq::o_rly::admin
