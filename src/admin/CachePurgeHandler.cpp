#include "admin/CachePurgeHandler.h"

#include <folly/CancellationToken.h>
#include <folly/io/IOBuf.h>
#include <folly/json/json.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/lib/http/HTTPMessage.h>

#include "MoqxRelayContext.h"
#include "admin/AdminServer.h"

namespace openmoq::moqx::admin {

void registerCachePurgeRoute(AdminServer& adminServer, std::shared_ptr<MoqxRelayContext> context) {
  adminServer.addRoute(
      "POST",
      "/cache/purge",
      [weak = std::weak_ptr<MoqxRelayContext>(context)](
          std::unique_ptr<proxygen::HTTPMessage> /*req*/,
          std::unique_ptr<folly::IOBuf> body,
          proxygen::ResponseHandler* downstream,
          folly::CancellationToken /*cancelToken*/
      ) {
        auto ctx = weak.lock();
        if (!ctx) {
          proxygen::ResponseBuilder(downstream)
              .status(503, proxygen::HTTPMessage::getDefaultReason(503))
              .body(folly::IOBuf::copyBuffer("{\"error\":\"server unavailable\"}\n"))
              .sendWithEOM();
          return;
        }

        // Parse optional JSON body: {"service":"...","namespace":"...","track":"..."}
        std::string serviceName;
        std::optional<moxygen::FullTrackName> ftn;
        std::optional<moxygen::TrackNamespace> nsOnly;
        if (body) {
          try {
            auto bodyStr = body->moveToFbString().toStdString();
            if (!bodyStr.empty()) {
              auto parsed = folly::parseJson(bodyStr);
              if (parsed.isObject()) {
                if (parsed.count("service")) {
                  serviceName = parsed["service"].asString();
                }
                if (parsed.count("namespace") && parsed.count("track")) {
                  auto ns = parsed["namespace"].asString();
                  auto track = parsed["track"].asString();
                  ftn = moxygen::FullTrackName{moxygen::TrackNamespace(ns, "/"), std::move(track)};
                } else if (parsed.count("namespace")) {
                  nsOnly = moxygen::TrackNamespace(parsed["namespace"].asString(), "/");
                }
              }
            }
          } catch (const std::exception& e) {
            proxygen::ResponseBuilder(downstream)
                .status(400, proxygen::HTTPMessage::getDefaultReason(400))
                .header("Content-Type", "application/json")
                .body(folly::IOBuf::copyBuffer("{\"error\":\"invalid JSON body\"}\n"))
                .sendWithEOM();
            return;
          }
        }

        auto result = nsOnly ? ctx->safePurgeNamespace(serviceName, *nsOnly)
                             : ctx->safePurge(serviceName, ftn);

        auto respBody = std::string("{\"evicted\":") + std::to_string(result.evicted) +
                        ",\"skipped\":" + std::to_string(result.skipped) + "}\n";
        proxygen::ResponseBuilder(downstream)
            .status(200, proxygen::HTTPMessage::getDefaultReason(200))
            .header("Content-Type", "application/json")
            .body(folly::IOBuf::copyBuffer(respBody))
            .sendWithEOM();
      }
  );
}

} // namespace openmoq::moqx::admin
