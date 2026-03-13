#include <o_rly/admin/AdminServer.h>

#include <folly/CancellationToken.h>
#include <folly/io/IOBufQueue.h>
#include <folly/logging/xlog.h>
#include <proxygen/httpserver/RequestHandler.h>
#include <proxygen/httpserver/RequestHandlerFactory.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/httpserver/ScopedHTTPServer.h>
#include <proxygen/lib/http/HTTPMessage.h>

namespace openmoq::o_rly::admin {

// ---------------------------------------------------------------------------
// AdminRequestHandler — generic request aggregator
//
// The only proxygen::RequestHandler subclass needed. Accumulates request
// headers and body, then dispatches to the RouteHandler on EOM.
// ---------------------------------------------------------------------------
class AdminRequestHandler : public proxygen::RequestHandler {
public:
  explicit AdminRequestHandler(RouteHandler handler) : handler_(std::move(handler)) {}

  void onRequest(std::unique_ptr<proxygen::HTTPMessage> headers) noexcept override {
    req_ = std::move(headers);
  }

  void onBody(std::unique_ptr<folly::IOBuf> body) noexcept override {
    body_.append(std::move(body));
  }

  void onEOM() noexcept override {
    handler_(std::move(req_), body_.move(), downstream_, cancellationSource_.getToken());
  }

  void onUpgrade(proxygen::UpgradeProtocol /*proto*/) noexcept override {}

  void requestComplete() noexcept override { delete this; }

  void onError(proxygen::ProxygenError /*err*/) noexcept override {
    cancellationSource_.requestCancellation();
    delete this;
  }

private:
  RouteHandler handler_;
  std::unique_ptr<proxygen::HTTPMessage> req_;
  folly::IOBufQueue body_{folly::IOBufQueue::cacheChainLength()};
  folly::CancellationSource cancellationSource_;
};

// ---------------------------------------------------------------------------
// AdminHandlerFactory — holds the route table and creates AdminRequestHandlers
// ---------------------------------------------------------------------------
class AdminHandlerFactory : public proxygen::RequestHandlerFactory {
public:
  explicit AdminHandlerFactory(std::vector<Route> routes) : routes_(std::move(routes)) {}

  void onServerStart(folly::EventBase* /*evb*/) noexcept override {}
  void onServerStop() noexcept override {}

  proxygen::RequestHandler*
  onRequest(proxygen::RequestHandler* /*upstream*/, proxygen::HTTPMessage* msg) noexcept override {
    const auto& method = msg->getMethodString();
    const auto& path = msg->getPath();

    for (const auto& route : routes_) {
      if (route.method == method && route.path == path) {
        return new AdminRequestHandler(route.handler);
      }
    }

    // Unknown route → 404
    return new AdminRequestHandler(
        [](auto /*req*/, auto /*body*/, auto* downstream, auto /*cancelToken*/) {
          proxygen::ResponseBuilder(downstream)
              .status(404, proxygen::HTTPMessage::getDefaultReason(404))
              .body(folly::IOBuf::copyBuffer("Not Found\n"))
              .sendWithEOM();
        }
    );
  }

private:
  std::vector<Route> routes_;
};

// ---------------------------------------------------------------------------
// AdminServer
// ---------------------------------------------------------------------------

AdminServer::AdminServer() = default;
AdminServer::~AdminServer() = default;

void AdminServer::addRoute(std::string method, std::string path, RouteHandler handler) {
  XCHECK(!httpServer_) << "addRoute() called after start()";
  routes_.push_back({std::move(method), std::move(path), std::move(handler)});
}

static constexpr int kAdminServerThreads = 1;

bool AdminServer::start(uint16_t port) {
  proxygen::HTTPServerOptions options;
  options.threads = kAdminServerThreads;
  options.handlerFactories.push_back(std::make_unique<AdminHandlerFactory>(routes_));

  proxygen::HTTPServer::IPConfig cfg{
      folly::SocketAddress("::", port),
      proxygen::HTTPServer::Protocol::HTTP
  };

  try {
    httpServer_ = proxygen::ScopedHTTPServer::start(std::move(cfg), std::move(options));
    return true;
  } catch (const std::exception& ex) {
    XLOG(ERR) << "AdminServer failed to start on port " << port << ": " << ex.what();
    return false;
  }
}

void AdminServer::stop() {
  httpServer_.reset();
}

} // namespace openmoq::o_rly::admin
