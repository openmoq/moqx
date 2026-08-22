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
#include <folly/coro/Task.h>
#include <folly/coro/WithCancellation.h>
#include <folly/executors/GlobalExecutor.h>
#include <folly/futures/Future.h>
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
constexpr size_t kChunkSize = 64 * 1024;

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

// Blocking; must run off the event-loop thread. Returns nullptr if the file
// cannot be opened, is not a regular file, is empty, or exceeds maxBytes.
// maxBytes is a policy limit on what a client may pull, not a memory bound:
// the body is streamed, so it never lands in the process whole.
std::unique_ptr<folly::File> openLogFile(const std::string& path, size_t maxBytes) {
  std::unique_ptr<folly::File> file;
  try {
    file = std::make_unique<folly::File>(path, O_RDONLY);
  } catch (const std::exception&) {
    return nullptr;
  }

  struct stat st{};
  if (::fstat(file->fd(), &st) != 0 || !S_ISREG(st.st_mode))
    return nullptr;
  const auto size = static_cast<size_t>(st.st_size);
  if (size == 0 || size > maxBytes)
    return nullptr;

  return file;
}

// Blocking; must run off the event-loop thread. Returns a zero-length buffer
// at EOF, nullptr on read error.
std::unique_ptr<folly::IOBuf> readChunk(int fd) {
  auto buf = folly::IOBuf::create(kChunkSize);
  const auto rc = folly::readNoInt(fd, buf->writableTail(), kChunkSize);
  if (rc < 0)
    return nullptr;
  buf->append(static_cast<size_t>(rc));
  return buf;
}

// Runs on the admin event base. Every resumption point must re-check
// cancelToken: downstream is destroyed as soon as cancellation fires.
folly::coro::Task<void> streamLogFile(
    std::string filePath,
    std::string fileName,
    proxygen::ResponseHandler* downstream,
    folly::CancellationToken cancelToken
) {
  if (cancelToken.isCancellationRequested())
    co_return;

  // Open on the global CPU pool: open(2)/fstat(2) block.
  auto openResult =
      co_await folly::coro::co_awaitTry(folly::via(folly::getGlobalCPUExecutor(), [&filePath] {
        return openLogFile(filePath, kMaxDownloadBytes);
      }));
  if (openResult.hasException()) {
    XLOG(ERR) << "ConnectionLogsHandler: file open threw: " << openResult.exception().what();
    if (!cancelToken.isCancellationRequested()) {
      sendError(downstream, 500, "internal error\n");
    }
    co_return;
  }

  if (cancelToken.isCancellationRequested())
    co_return;

  std::unique_ptr<folly::File> file = std::move(openResult.value());
  if (!file) {
    sendError(downstream, 404, "log file not found or exceeds size limit\n");
    co_return;
  }

  // No Content-Length: the log is still being appended to while we read it, so
  // a length from fstat(2) would be stale by EOF. The body streams chunked.
  proxygen::ResponseBuilder(downstream)
      .status(200, proxygen::HTTPMessage::getDefaultReason(200))
      .header("Content-Type", "application/json")
      .header("Content-Disposition", "attachment; filename=\"" + fileName + "\"")
      .send();

  // Read on the CPU pool, send from the event-loop thread: each disk read
  // overlaps the network write of the chunk before it.
  for (;;) {
    auto chunkResult = co_await folly::coro::co_awaitTry(
        folly::via(folly::getGlobalCPUExecutor(), [fd = file->fd()] { return readChunk(fd); })
    );

    if (cancelToken.isCancellationRequested())
      co_return;

    if (chunkResult.hasException() || !chunkResult.value()) {
      // Headers are already out, so the only way left to signal failure is to
      // tear the response down.
      XLOG(ERR) << "ConnectionLogsHandler: read failed for " << filePath;
      downstream->sendAbort();
      co_return;
    }

    auto chunk = std::move(chunkResult.value());
    if (chunk->empty()) {
      proxygen::ResponseBuilder(downstream).sendWithEOM();
      co_return;
    }
    proxygen::ResponseBuilder(downstream).body(std::move(chunk)).send();
  }
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
          folly::CancellationToken cancelToken,
          const std::shared_ptr<EgressGate>& /*egress*/
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
                streamLogFile(
                    std::move(filePath),
                    std::move(fileName),
                    downstream,
                    std::move(cancelToken)
                )
            )
        )
            .start();
      }
  );
}

} // namespace openmoq::moqx::admin
