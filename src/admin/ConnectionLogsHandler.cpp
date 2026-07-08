/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "admin/ConnectionLogsHandler.h"

#include <cctype>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <sys/stat.h>

#include <folly/CancellationToken.h>
#include <folly/File.h>
#include <folly/FileUtil.h>
#include <folly/coro/Invoke.h>
#include <folly/coro/Task.h>
#include <folly/coro/WithCancellation.h>
#include <folly/executors/GlobalExecutor.h>
#include <folly/io/IOBuf.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/logging/xlog.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/lib/http/HTTPMessage.h>

#include "admin/AdminResponse.h"
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
  folly::File file;
  try {
    file = folly::File(path, O_RDONLY);
  } catch (const std::exception&) {
    return nullptr;
  }

  struct stat st{};
  if (::fstat(file.fd(), &st) != 0 || !S_ISREG(st.st_mode))
    return nullptr;
  const auto size = static_cast<size_t>(st.st_size);
  if (size == 0 || size > maxBytes)
    return nullptr;

  auto content = std::make_unique<std::string>();
  if (!folly::readFile(file.fd(), *content, size) || content->size() != size)
    return nullptr;

  auto* data = content->data();
  const auto len = content->size();
  return folly::IOBuf::takeOwnership(
      data,
      len,
      [](void*, void* userData) { delete static_cast<std::string*>(userData); },
      content.release()
  );
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
          sendError(downstream, 400, "type must be 'mlog' or 'qlog'\n");
          return;
        }

        if (dir->empty()) {
          sendError(downstream, 503, "that log type is not configured\n");
          return;
        }

        const auto& rawCid = req->getQueryParam("connection_id");
        if (rawCid.empty()) {
          sendError(downstream, 400, "missing connection_id\n");
          return;
        }

        auto normCid = normalizeConnectionId(rawCid);
        if (!normCid) {
          sendError(downstream, 400, "invalid connection_id\n");
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
                [filePath = std::move(filePath),
                 fileName = std::move(fileName),
                 downstream = downstream,
                 cancelToken = cancelToken]() -> folly::coro::Task<void> {
                  if (cancelToken.isCancellationRequested())
                    co_return;

                  // Read the file on the global CPU pool to avoid blocking the admin
                  // event-loop thread.
                  auto readResult = co_await folly::coro::co_awaitTry(folly::coro::co_withExecutor(
                      folly::getGlobalCPUExecutor(),
                      folly::coro::co_invoke(
                          [&filePath]() -> folly::coro::Task<std::unique_ptr<folly::IOBuf>> {
                            co_return readFileToIOBuf(filePath, kMaxDownloadBytes);
                          }
                      )
                  ));
                  if (readResult.hasException()) {
                    XLOG(ERR) << "ConnectionLogsHandler: file read threw: "
                              << readResult.exception().what();
                    if (!cancelToken.isCancellationRequested()) {
                      sendError(downstream, 500, "internal error\n");
                    }
                    co_return;
                  }

                  if (cancelToken.isCancellationRequested())
                    co_return;

                  std::unique_ptr<folly::IOBuf> fileBuf = std::move(readResult.value());
                  if (!fileBuf) {
                    sendError(downstream, 404, "log file not found or exceeds size limit\n");
                    co_return;
                  }

                  proxygen::ResponseBuilder(downstream)
                      .status(200, proxygen::HTTPMessage::getDefaultReason(200))
                      .header("Content-Type", "application/json")
                      .header("Content-Disposition", "attachment; filename=\"" + fileName + "\"")
                      .body(std::move(fileBuf))
                      .sendWithEOM();
                }()
            )
        )
            .start();
      }
  );
}

} // namespace openmoq::moqx::admin
