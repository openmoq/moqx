/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "admin/QLogCaptureHandler.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <vector>

#include <sys/stat.h>

#include <folly/Conv.h>
#include <folly/io/IOBuf.h>
#include <folly/json/dynamic.h>
#include <folly/json/json.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/lib/http/HTTPMessage.h>

#include "admin/AdminResponse.h"
#include "admin/AdminServer.h"

namespace openmoq::moqx::admin {

namespace {

using logging::QLogCapture;

constexpr size_t kMaxListedFiles = 100;

struct QLogFile {
  std::string connectionId;
  int64_t bytes;
  int64_t modified;
};

// Newest first, at most kMaxListedFiles.
std::vector<QLogFile> listQLogFiles(const std::string& dir) {
  std::vector<QLogFile> files;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    const auto& path = entry.path();
    if (path.extension() != ".qlog") {
      continue;
    }
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
      continue;
    }
    files.push_back({path.stem().string(), static_cast<int64_t>(st.st_size), st.st_mtime});
  }
  const auto kept = std::min(files.size(), kMaxListedFiles);
  std::partial_sort(
      files.begin(),
      files.begin() + kept,
      files.end(),
      [](const auto& a, const auto& b) { return a.modified > b.modified; }
  );
  files.resize(kept);
  return files;
}

folly::dynamic statusJson(const QLogCapture::Status& status) {
  const auto expires =
      std::chrono::duration_cast<std::chrono::seconds>(status.expiresAt.time_since_epoch()).count();
  return folly::dynamic::object("armed", status.armed)(
      "mode", std::string(QLogCapture::modeName(status.mode))
  )("remaining", status.remaining)("captured", status.captured)(
      "expires_at", expires > 0 ? folly::dynamic(expires) : folly::dynamic(nullptr)
  );
}

void sendJson(proxygen::ResponseHandler* downstream, const folly::dynamic& body) {
  proxygen::ResponseBuilder(downstream)
      .status(200, proxygen::HTTPMessage::getDefaultReason(200))
      .header("Content-Type", "application/json")
      .body(folly::IOBuf::copyBuffer(folly::toJson(body) + "\n"))
      .sendWithEOM();
}

// nullopt when present but not an integer within [1, max].
std::optional<uint32_t> boundedParam(
    const proxygen::HTTPMessage& req,
    const std::string& name,
    uint32_t defaultValue,
    uint32_t max
) {
  if (!req.hasQueryParam(name)) {
    return defaultValue;
  }
  auto value = folly::tryTo<uint32_t>(req.getDecodedQueryParam(name));
  if (!value || *value < 1 || *value > max) {
    return std::nullopt;
  }
  return *value;
}

} // namespace

void registerQLogCaptureRoutes(
    AdminServer& adminServer,
    std::shared_ptr<QLogCapture> capture,
    std::string qlogDir
) {
  static const std::string kNotConfigured = "qlog is not configured (logging.qlog.dir)\n";

  adminServer.addRoute(
      "POST",
      "/qlog/capture",
      [capture](
          std::unique_ptr<proxygen::HTTPMessage> req,
          std::unique_ptr<folly::IOBuf> /*body*/,
          proxygen::ResponseHandler* downstream,
          folly::CancellationToken /*cancelToken*/,
          const std::shared_ptr<EgressGate>& /*egress*/
      ) {
        if (!capture) {
          sendError(downstream, 503, kNotConfigured);
          return;
        }
        auto count = boundedParam(*req, "count", 1, QLogCapture::kMaxCount);
        if (!count) {
          sendError(
              downstream,
              400,
              folly::to<std::string>("count must be 1-", QLogCapture::kMaxCount, "\n")
          );
          return;
        }
        auto seconds = boundedParam(
            *req,
            "seconds",
            60,
            static_cast<uint32_t>(QLogCapture::kMaxDuration.count())
        );
        if (!seconds) {
          sendError(
              downstream,
              400,
              folly::to<std::string>("seconds must be 1-", QLogCapture::kMaxDuration.count(), "\n")
          );
          return;
        }
        auto mode = QLogCapture::Mode::Cc;
        if (req->hasQueryParam("mode")) {
          auto parsed = QLogCapture::parseMode(req->getDecodedQueryParam("mode"));
          if (!parsed) {
            sendError(downstream, 400, "mode must be cc or full\n");
            return;
          }
          mode = *parsed;
        }
        capture->arm(*count, std::chrono::seconds(*seconds), mode);
        sendJson(downstream, statusJson(capture->status()));
      }
  );

  adminServer.addRoute(
      "DELETE",
      "/qlog/capture",
      [capture](
          std::unique_ptr<proxygen::HTTPMessage> /*req*/,
          std::unique_ptr<folly::IOBuf> /*body*/,
          proxygen::ResponseHandler* downstream,
          folly::CancellationToken /*cancelToken*/,
          const std::shared_ptr<EgressGate>& /*egress*/
      ) {
        if (!capture) {
          sendError(downstream, 503, kNotConfigured);
          return;
        }
        capture->disarm();
        sendJson(downstream, statusJson(capture->status()));
      }
  );

  adminServer.addRoute(
      "GET",
      "/qlog/capture",
      [capture, qlogDir = std::move(qlogDir)](
          std::unique_ptr<proxygen::HTTPMessage> /*req*/,
          std::unique_ptr<folly::IOBuf> /*body*/,
          proxygen::ResponseHandler* downstream,
          folly::CancellationToken /*cancelToken*/,
          const std::shared_ptr<EgressGate>& /*egress*/
      ) {
        if (!capture) {
          sendError(downstream, 503, kNotConfigured);
          return;
        }
        auto body = statusJson(capture->status());
        auto files = folly::dynamic::array();
        for (const auto& f : listQLogFiles(qlogDir)) {
          files.push_back(folly::dynamic::object("connection_id", f.connectionId)("bytes", f.bytes)(
              "modified",
              f.modified
          ));
        }
        body["dir"] = qlogDir;
        body["files"] = std::move(files);
        sendJson(downstream, body);
      }
  );
}

} // namespace openmoq::moqx::admin
