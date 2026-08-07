/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "admin/ConnectionLogsHandler.h"

#include <cctype>
#include <fstream>
#include <string>
#include <string_view>

#include <folly/CancellationToken.h>
#include <folly/coro/Task.h>
#include <folly/coro/WithCancellation.h>
#include <folly/executors/GlobalExecutor.h>
#include <folly/io/IOBuf.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/logging/xlog.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/lib/http/HTTPMessage.h>

#include "admin/AdminServer.h"

namespace openmoq::moqx::admin {

namespace {

constexpr size_t kMaxDownloadBytes = 512ULL * 1024 * 1024; // 512 MB hard cap

// Normalize a raw connection ID string:
//   - strip 0x/0X prefix
//   - lowercase hex digits
//   - validate hex-only, 1–40 chars
std::optional<std::string> normalizeConnectionId(std::string_view raw) {
  if (raw.size() >= 2 && raw[0] == '0' && (raw[1] == 'x' || raw[1] == 'X')) {
    raw.remove_prefix(2);
  }
  std::string result;
  result.reserve(raw.size());
  for (char c : raw) {
    if (!std::isxdigit(static_cast<unsigned char>(c)))
      return std::nullopt;
    result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (result.empty() || result.size() > 40)
    return std::nullopt;
  return result;
}

// Read an entire file into an IOBuf. Returns nullptr if the file cannot be
// opened, is empty, or exceeds maxBytes.
std::unique_ptr<folly::IOBuf> readFileToIOBuf(const std::string& path, size_t maxBytes) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f.is_open())
    return nullptr;
  const auto size = static_cast<size_t>(f.tellg());
  if (size == 0 || size > maxBytes)
    return nullptr;
  f.seekg(0);
  auto buf = folly::IOBuf::create(size);
  f.read(reinterpret_cast<char*>(buf->writableData()), static_cast<std::streamsize>(size));
  if (!f)
    return nullptr;
  buf->append(size);
  return buf;
}

folly::coro::Task<std::unique_ptr<folly::IOBuf>> readFileTask(std::string path, size_t maxBytes) {
  co_return readFileToIOBuf(path, maxBytes);
}

} // namespace

void registerConnectionLogsRoutes(
    AdminServer& adminServer,
    const std::optional<config::LoggingConfig>& logging
) {
  std::string mlogDir, qlogDir;
  if (logging) {
    if (logging->mlog && !logging->mlog->dir.empty()) {
      mlogDir = logging->mlog->dir;
    }
    if (logging->qlog && !logging->qlog->dir.empty()) {
      qlogDir = logging->qlog->dir;
    }
  }

  // ── GET /logs?connection_id=<hex>&type=mlog|qlog
  //
  // Path is constructed directly as {dir}/{normalized_cid}.{ext}
  adminServer.addRoute(
      "GET",
      "/logs",
      [mlogDir = std::move(mlogDir), qlogDir = std::move(qlogDir)](
          std::unique_ptr<proxygen::HTTPMessage> req,
          std::unique_ptr<folly::IOBuf> /*body*/,
          proxygen::ResponseHandler* downstream,
          folly::CancellationToken cancelToken
      ) {
        // Resolve type → directory, file extension, Content-Type.
        const auto& typeStr = req->getQueryParam("type");
        const std::string* dir = nullptr;
        const char* ext = nullptr;
        if (typeStr == "mlog") {
          dir = &mlogDir;
          ext = ".mlog";
        } else if (typeStr == "qlog") {
          dir = &qlogDir;
          ext = ".qlog";
        } else {
          proxygen::ResponseBuilder(downstream)
              .status(400, proxygen::HTTPMessage::getDefaultReason(400))
              .header("Content-Type", "application/json")
              .body(folly::IOBuf::copyBuffer("{\"error\":\"type must be 'mlog' or 'qlog'\"}\n"))
              .sendWithEOM();
          return;
        }

        if (dir->empty()) {
          proxygen::ResponseBuilder(downstream)
              .status(503, proxygen::HTTPMessage::getDefaultReason(503))
              .header("Content-Type", "application/json")
              .body(folly::IOBuf::copyBuffer("{\"error\":\"that log type is not configured\"}\n"))
              .sendWithEOM();
          return;
        }

        const auto& rawCid = req->getQueryParam("connection_id");
        if (rawCid.empty()) {
          proxygen::ResponseBuilder(downstream)
              .status(400, proxygen::HTTPMessage::getDefaultReason(400))
              .header("Content-Type", "application/json")
              .body(folly::IOBuf::copyBuffer("{\"error\":\"missing connection_id\"}\n"))
              .sendWithEOM();
          return;
        }

        auto normCid = normalizeConnectionId(rawCid);
        if (!normCid) {
          proxygen::ResponseBuilder(downstream)
              .status(400, proxygen::HTTPMessage::getDefaultReason(400))
              .header("Content-Type", "application/json")
              .body(folly::IOBuf::copyBuffer("{\"error\":\"invalid connection_id\"}\n"))
              .sendWithEOM();
          return;
        }

        // {dir}/{normalizedCid}.{ext}
        auto filePath = *dir + "/" + *normCid + ext;
        auto fileName = *normCid + ext;

        auto* evb = folly::EventBaseManager::get()->getEventBase();
        folly::coro::co_withCancellation(
            cancelToken,
            folly::coro::co_withExecutor(
                evb,
                [](auto path, auto name, auto* ds, auto token) -> folly::coro::Task<void> {
                  if (token.isCancellationRequested())
                    co_return;

                  // Read the file on the global CPU pool to avoid blocking
                  // the admin event-loop thread.
                  std::unique_ptr<folly::IOBuf> fileBuf;
                  try {
                    fileBuf = co_await folly::coro::co_withExecutor(
                        folly::getGlobalCPUExecutor(),
                        readFileTask(std::move(path), kMaxDownloadBytes)
                    );
                  } catch (const std::exception& e) {
                    XLOG(ERR) << "ConnectionLogsHandler: file read threw: " << e.what();
                    if (!token.isCancellationRequested()) {
                      proxygen::ResponseBuilder(ds)
                          .status(500, proxygen::HTTPMessage::getDefaultReason(500))
                          .header("Content-Type", "application/json")
                          .body(folly::IOBuf::copyBuffer("{\"error\":\"internal error\"}\n"))
                          .sendWithEOM();
                    }
                    co_return;
                  }

                  if (token.isCancellationRequested())
                    co_return;

                  if (!fileBuf) {
                    proxygen::ResponseBuilder(ds)
                        .status(404, proxygen::HTTPMessage::getDefaultReason(404))
                        .header("Content-Type", "application/json")
                        .body(folly::IOBuf::copyBuffer(
                            "{\"error\":\"log file not found or exceeds size limit\"}\n"
                        ))
                        .sendWithEOM();
                    co_return;
                  }

                  proxygen::ResponseBuilder(ds)
                      .status(200, proxygen::HTTPMessage::getDefaultReason(200))
                      .header("Content-Type", "application/json")
                      .header("Content-Disposition", "attachment; filename=\"" + name + "\"")
                      .body(std::move(fileBuf))
                      .sendWithEOM();
                }(std::move(filePath), std::move(fileName), downstream, cancelToken)
            )
        )
            .start();
      }
  );
}

} // namespace openmoq::moqx::admin
