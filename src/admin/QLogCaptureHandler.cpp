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

#include <folly/CancellationToken.h>
#include <folly/Conv.h>
#include <folly/coro/Task.h>
#include <folly/coro/WithCancellation.h>
#include <folly/executors/GlobalExecutor.h>
#include <folly/futures/Future.h>
#include <folly/io/Cursor.h>
#include <folly/io/IOBuf.h>
#include <folly/io/IOBufQueue.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/logging/xlog.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/lib/http/HTTPMessage.h>

#include "admin/AdminResponse.h"
#include "admin/AdminServer.h"
#include "admin/JsonWriter.h"

namespace openmoq::moqx::admin {

namespace {

using logging::QLogCapture;

constexpr size_t kMaxListedFiles = 100;
constexpr size_t kMaxScannedEntries = 10000;

struct QLogFile {
  std::string connectionId;
  int64_t bytes;
  int64_t modified;
};

struct QLogListing {
  std::vector<QLogFile> files; // newest first
  bool truncated{false};       // stopped after kMaxScannedEntries
};

// Blocking; must run off the event-loop thread. Keeps the newest
// kMaxListedFiles of at most kMaxScannedEntries directory entries.
QLogListing listQLogFiles(const std::string& dir) {
  QLogListing listing;
  auto& files = listing.files;
  // Heap order puts the oldest kept file at the front.
  const auto newer = [](const QLogFile& a, const QLogFile& b) { return a.modified > b.modified; };
  size_t scanned = 0;
  std::error_code ec;
  for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (++scanned > kMaxScannedEntries) {
      listing.truncated = true;
      break;
    }
    const auto& path = it->path();
    if (path.extension() != ".qlog") {
      continue;
    }
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
      continue;
    }
    QLogFile file{path.stem().string(), static_cast<int64_t>(st.st_size), st.st_mtime};
    if (files.size() < kMaxListedFiles) {
      files.push_back(std::move(file));
      std::push_heap(files.begin(), files.end(), newer);
    } else if (newer(file, files.front())) {
      std::pop_heap(files.begin(), files.end(), newer);
      files.back() = std::move(file);
      std::push_heap(files.begin(), files.end(), newer);
    }
  }
  std::sort_heap(files.begin(), files.end(), newer);
  return listing;
}

// Capture status; with a listing, also its files.
std::unique_ptr<folly::IOBuf>
statusBody(const QLogCapture::Status& status, const QLogListing* listing = nullptr) {
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
  if (listing) {
    w.key("files");
    w.beginArray();
    for (const auto& f : listing->files) {
      w.beginObject();
      w.field("connection_id", f.connectionId);
      w.field("bytes", f.bytes);
      w.field("modified", f.modified);
      w.endObject();
    }
    w.endArray();
    w.field("truncated", listing->truncated);
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

// Runs on the admin event base. downstream is destroyed as soon as
// cancelToken fires, so it is re-checked after the scan.
folly::coro::Task<void> sendStatusWithFiles(
    std::shared_ptr<QLogCapture> capture,
    std::string dir,
    proxygen::ResponseHandler* downstream,
    folly::CancellationToken cancelToken
) {
  // dir by reference: GCC 11 destroys temporaries in a co_await operand twice.
  auto listing = co_await folly::coro::co_awaitTry(
      folly::via(folly::getGlobalCPUExecutor(), [&dir] { return listQLogFiles(dir); })
  );
  if (cancelToken.isCancellationRequested()) {
    co_return;
  }
  if (listing.hasException()) {
    XLOG(ERR) << "QLogCaptureHandler: listing threw: " << listing.exception().what();
    sendError(downstream, 500, "internal error\n");
    co_return;
  }
  sendJson(downstream, statusBody(capture->status(), &listing.value()));
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
        auto replace = boolQueryParam(*req, "replace", false);
        if (!replace) {
          sendError(downstream, 400, "replace must be one of 1, 0, true, false\n");
          return;
        }
        if (!capture->arm(*count, std::chrono::seconds(*seconds), mode, *replace)) {
          sendError(downstream, 409, "a capture is in progress; pass replace=1 to replace it\n");
          return;
        }
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
          folly::CancellationToken cancelToken,
          const std::shared_ptr<EgressGate>& /*egress*/
      ) {
        if (!capture) {
          sendError(downstream, 503, kNotConfigured);
          return;
        }
        auto* evb = folly::EventBaseManager::get()->getEventBase();
        folly::coro::co_withCancellation(
            cancelToken,
            folly::coro::co_withExecutor(
                evb,
                sendStatusWithFiles(capture, qlogDir, downstream, cancelToken)
            )
        )
            .start();
      }
  );
}

} // namespace openmoq::moqx::admin
