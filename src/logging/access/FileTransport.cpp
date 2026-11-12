/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "logging/access/FileTransport.h"

#include <fcntl.h>
#include <limits.h>
#include <sys/uio.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <folly/FileUtil.h>
#include <folly/logging/xlog.h>

namespace openmoq::moqx::logging {

namespace {

constexpr std::chrono::milliseconds kRetryAfter{1000};

folly::Expected<folly::File, std::string> openAppend(const std::string& path) {
  auto file = folly::File::makeFile(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (file.hasError()) {
    return folly::makeUnexpected(file.error().what().toStdString());
  }
  return std::move(file.value());
}

// On an error, reports the bytes already written so that the next write starts after them.
AccessLogWriteResult writeChain(int fd, const folly::IOBuf& data) {
  auto iov = data.getIov();
  size_t sent = 0;
  size_t idx = 0;
  while (idx < iov.size()) {
    auto count = int(std::min(iov.size() - idx, size_t(IOV_MAX)));
    auto n = folly::writevNoInt(fd, iov.data() + idx, count);
    if (n <= 0) {
      XLOG_EVERY_MS(ERR, 10000) << "access log write failed: "
                                << (n < 0 ? std::strerror(errno) : "wrote 0 bytes");
      return {AccessLogWriteStatus::Retry, kRetryAfter, sent};
    }
    sent += size_t(n);
    auto left = size_t(n);
    while (idx < iov.size() && left >= iov[idx].iov_len) {
      left -= iov[idx].iov_len;
      ++idx;
    }
    if (left) {
      iov[idx].iov_base = static_cast<char*>(iov[idx].iov_base) + left;
      iov[idx].iov_len -= left;
    }
  }
  return {};
}

} // namespace

folly::Expected<std::unique_ptr<FileTransport>, std::string> FileTransport::open(std::string path) {
  auto file = openAppend(path);
  if (file.hasError()) {
    return folly::makeUnexpected(std::move(file.error()));
  }
  return std::unique_ptr<FileTransport>(new FileTransport(std::move(path), std::move(*file)));
}

std::unique_ptr<FileTransport> FileTransport::standardOutput() {
  return std::unique_ptr<FileTransport>(
      new FileTransport(std::string(), folly::File(STDOUT_FILENO, /*ownsFd=*/false))
  );
}

folly::coro::Task<AccessLogWriteResult> FileTransport::write(const folly::IOBuf& data) {
  if (!file_) {
    co_return AccessLogWriteResult{AccessLogWriteStatus::Retry, kRetryAfter};
  }
  co_return writeChain(file_.fd(), data);
}

void FileTransport::reopen() {
  if (path_.empty()) {
    return;
  }
  auto file = openAppend(path_);
  if (file.hasError()) {
    XLOG(ERR) << "access log reopen of " << path_ << " failed: " << file.error();
    return;
  }
  file_ = std::move(*file);
}

folly::coro::Task<void> FileTransport::close() {
  file_.closeNoThrow();
  co_return;
}

} // namespace openmoq::moqx::logging
