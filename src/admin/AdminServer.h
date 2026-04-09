#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <folly/CancellationToken.h>
#include <folly/io/IOBuf.h>

#include "config/config.h"

// Forward declarations
namespace proxygen {
class ScopedHTTPServer;
class HTTPMessage;
class ResponseHandler;
} // namespace proxygen

namespace openmoq::moqx::admin {

// Route handler: receives the complete request (headers + body) and owns the
// response. May respond asynchronously — launch a coroutine and send via
// downstream later. cancelToken is signalled if the client disconnects before
// the response is produced (onError fired on the underlying RequestHandler).
using RouteHandler = std::function<void(
    std::unique_ptr<proxygen::HTTPMessage> req,
    std::unique_ptr<folly::IOBuf> body,
    proxygen::ResponseHandler* downstream,
    folly::CancellationToken cancelToken
)>;

struct Route {
  std::string method;
  std::string path;
  RouteHandler handler;
};

// Wraps proxygen::HTTPServer with a simple method+path route table.
//
// Call addRoute() before start(); call stop() on shutdown.
//
// Async handlers (e.g. Prometheus scrape) launch a coroutine from within
// the RouteHandler and send the response asynchronously via downstream.
//
// Note: Uses proxygen/httpserver/HTTPServer.h (classic RequestHandler API).
// Future: migrate to proxygen/lib/http/coro/server/HTTPServer.h.
// Future: TLS/mTLS via wangle::SSLContextConfig (PR #29 config).
// Future routes: GET /sessions for live session inspection.
class AdminServer {
public:
  AdminServer();
  ~AdminServer();

  // Register a route. Must be called before start(); CHECKs if called after.
  void addRoute(std::string method, std::string path, RouteHandler handler);

  // Start the admin server. Blocks until the server is ready to accept
  // connections (or fails). Returns true on success.
  bool start(const config::AdminConfig& config);

  // Stop the admin server.
  void stop();

private:
  std::vector<Route> routes_;
  std::unique_ptr<proxygen::ScopedHTTPServer> httpServer_;
};

} // namespace openmoq::moqx::admin
