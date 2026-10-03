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
#include <folly/io/Cursor.h>
#include <folly/io/IOBuf.h>
#include <folly/io/IOBufQueue.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/lib/http/HTTPMessage.h>

#include "admin/AdminResponse.h"
#include "admin/AdminServer.h"
#include "admin/JsonWriter.h"

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

// Capture status; with a directory, also the newest qlog files in it.
std::unique_ptr<folly::IOBuf>
statusBody(const QLogCapture::Status& status, const std::string* dir = nullptr) {
  folly::IOBufQueue queue{folly::IOBufQueue::cacheChainLength()};
  folly::io::QueueAppender app{&queue, 1024};
  JsonWriter w{app};
  w.beginObject();
  w.field("armed", status.armed);
  w.field("mode", QLogCapture::modeName(status.mode));
  w.field("remaining", uint64_t{status.remaining});
  w.field("captured", uint64_t{status.captured});
  const int64_t expires =
      std::chrono::duration_cast<std::chrono::seconds>(status.expiresAt.time_since_epoch()).count();
  w.key("expires_at");
  if (expires > 0) {
    w.intVal(expires);
  } else {
    w.nullVal();
  }
  if (dir) {
    w.field("dir", *dir);
    w.key("files");
    w.beginArray();
    for (const auto& f : listQLogFiles(*dir)) {
      w.beginObject();
      w.field("connection_id", f.connectionId);
      w.field("bytes", f.bytes);
      w.field("modified", f.modified);
      w.endObject();
    }
    w.endArray();
  }
  w.endObject();
  app.write(static_cast<uint8_t>('\n'));
  return queue.move();
}

void sendJson(proxygen::ResponseHandler* downstream, std::unique_ptr<folly::IOBuf> body) {
  proxygen::ResponseBuilder(downstream)
      .status(200, proxygen::HTTPMessage::getDefaultReason(200))
      .header("Content-Type", "application/json")
      .body(std::move(body))
      .sendWithEOM();
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
        auto count = boundedQueryParam(*req, "count", 1, QLogCapture::kMaxCount);
        if (!count) {
          sendError(
              downstream,
              400,
              folly::to<std::string>("count must be 1-", QLogCapture::kMaxCount, "\n")
          );
          return;
        }
        auto seconds = boundedQueryParam(
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
        sendJson(downstream, statusBody(capture->status()));
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
        sendJson(downstream, statusBody(capture->status()));
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
        sendJson(downstream, statusBody(capture->status(), &qlogDir));
      }
  );
}

} // namespace openmoq::moqx::admin
