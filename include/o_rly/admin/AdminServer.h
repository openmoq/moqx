#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <folly/io/IOBuf.h>

// Forward declarations
namespace proxygen {
class ScopedHTTPServer;
class HTTPMessage;
class ResponseHandler;
} // namespace proxygen

namespace openmoq::o_rly::admin {

// Route handler: receives the complete request (headers + body) and owns the
// response. May respond asynchronously — launch a coroutine and send via
// downstream later.
using RouteHandler = std::function<void(
    std::unique_ptr<proxygen::HTTPMessage> req,
    std::unique_ptr<folly::IOBuf> body,
    proxygen::ResponseHandler* downstream
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

  // Register a route. Must be called before start().
  void addRoute(std::string method, std::string path, RouteHandler handler);

  // Start the HTTP admin server on the given port. Blocks until the server is
  // ready to accept connections (or fails). Returns true on success.
  bool start(uint16_t port);

  // Stop the admin server.
  void stop();

private:
  std::vector<Route> routes_;
  std::unique_ptr<proxygen::ScopedHTTPServer> httpServer_;
};

} // namespace openmoq::o_rly::admin
