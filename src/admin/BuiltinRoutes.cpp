#include <o_rly/admin/BuiltinRoutes.h>

#include <folly/io/IOBuf.h>
#include <proxygen/httpserver/ResponseBuilder.h>

#include <o_rly/admin/AdminServer.h>

namespace openmoq::o_rly::admin {

void registerBuiltinRoutes(AdminServer& server) {
  server.addRoute("GET", "/info", [](auto /*req*/, auto /*body*/, auto* downstream) {
    proxygen::ResponseBuilder(downstream)
        .status(200, "OK")
        .header("Content-Type", "application/json")
        .body(folly::IOBuf::copyBuffer("{\"service\":\"o-rly\",\"version\":\"0.1.0\"}\n"))
        .sendWithEOM();
  });
}

} // namespace openmoq::o_rly::admin
