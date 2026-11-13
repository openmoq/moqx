/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "logging/access/FileTransport.h"

#include <fcntl.h>
#include <limits.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <folly/FileUtil.h>
#include <folly/coro/Sleep.h>
#include <folly/logging/xlog.h>

namespace openmoq::moqx::logging {

namespace {

constexpr std::chrono::milliseconds kRetryAfter{1000};
// The poll interval while a full pipe or socket has no room.
constexpr std::chrono::milliseconds kBlockedPoll{10};

folly::Expected<folly::File, std::string> openAppend(const std::string& path) {
  // The group can read the log, for a log shipper. An existing file keeps its mode.
  auto file = folly::File::makeFile(
      path.c_str(),
      O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NONBLOCK,
      0640
  );
  if (file.hasError()) {
    return folly::makeUnexpected(file.error().what().toStdString());
  }
  return std::move(file.value());
}

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_DONTWAIT | MSG_NOSIGNAL;
#else
constexpr int kSendFlags = MSG_DONTWAIT;
#endif

enum class ChainResult { Done, Blocked, Failed };

// Writes data from byte `sent` on, and adds what it writes to sent.
ChainResult writeChain(int fd, bool socket, const folly::IOBuf& data, size_t& sent) {
  auto iov = data.getIov();
  size_t idx = 0;
  auto skip = [&](size_t bytes) {
    while (idx < iov.size() && bytes >= iov[idx].iov_len) {
      bytes -= iov[idx].iov_len;
      ++idx;
    }
    if (bytes) {
      iov[idx].iov_base = static_cast<char*>(iov[idx].iov_base) + bytes;
      iov[idx].iov_len -= bytes;
    }
  };
  skip(sent);
  while (idx < iov.size()) {
    auto count = std::min(iov.size() - idx, size_t(IOV_MAX));
    ssize_t n;
    if (socket) {
      msghdr msg{};
      msg.msg_iov = iov.data() + idx;
      msg.msg_iovlen = static_cast<decltype(msg.msg_iovlen)>(count);
      do {
        n = ::sendmsg(fd, &msg, kSendFlags);
      } while (n < 0 && errno == EINTR);
    } else {
      n = folly::writevNoInt(fd, iov.data() + idx, int(count));
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return ChainResult::Blocked;
    }
    if (n <= 0) {
      XLOG_EVERY_MS(ERR, 10000) << "access log write failed: "
                                << (n < 0 ? std::strerror(errno) : "wrote 0 bytes");
      return ChainResult::Failed;
    }
    sent += size_t(n);
    skip(size_t(n));
  }
  return ChainResult::Done;
}

// On Linux, a FIFO on fd 1 is reopened as its own file description, so that O_NONBLOCK
// stays off the description that the parent shares. A socket sends with MSG_DONTWAIT. Any
// other fd 1 keeps blocking writes. If its reader stops, the writer thread blocks. At
// shutdown the relay's watchdog then exits the process after 10 seconds.
folly::File openStandardOutput(const struct stat& st) {
#ifdef __linux__
  if (S_ISFIFO(st.st_mode)) {
    int fd = ::open("/proc/self/fd/1", O_WRONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd >= 0) {
      return folly::File(fd, /*ownsFd=*/true);
    }
  }
#else
  (void)st;
#endif
  return folly::File(STDOUT_FILENO, /*ownsFd=*/false);
}

// A write to a full pipe or socket polls for room, and the stop deadline cancels the poll.
folly::coro::Task<AccessLogWriteResult>
writeTo(const folly::File& file, bool socket, const folly::IOBuf& data) {
  if (!file) {
    co_return AccessLogWriteResult{AccessLogWriteStatus::Retry, kRetryAfter};
  }
  size_t sent = 0;
  while (true) {
    switch (writeChain(file.fd(), socket, data, sent)) {
    case ChainResult::Done:
      co_return AccessLogWriteResult{};
    case ChainResult::Failed:
      co_return AccessLogWriteResult{AccessLogWriteStatus::Retry, kRetryAfter, sent};
    case ChainResult::Blocked:
      break;
    }
    auto slept = co_await folly::coro::co_awaitTry(folly::coro::sleep(kBlockedPoll));
    if (slept.hasException()) {
      co_return AccessLogWriteResult{AccessLogWriteStatus::Retry, kRetryAfter, sent};
    }
  }
}

} // namespace

folly::Expected<std::unique_ptr<FileTransport>, std::string> FileTransport::open(std::string path) {
  auto file = openAppend(path);
  if (file.hasError()) {
    return folly::makeUnexpected(std::move(file.error()));
  }
  return std::unique_ptr<FileTransport>(new FileTransport(std::move(path), std::move(*file)));
}

std::unique_ptr<StdoutTransport> StdoutTransport::open() {
  struct stat st{};
  ::fstat(STDOUT_FILENO, &st);
  return std::unique_ptr<StdoutTransport>(
      new StdoutTransport(openStandardOutput(st), S_ISSOCK(st.st_mode))
  );
}

folly::coro::Task<AccessLogWriteResult> FileTransport::write(const folly::IOBuf& data) {
  return writeTo(file_, /*socket=*/false, data);
}

void FileTransport::reopen() {
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

folly::coro::Task<AccessLogWriteResult> StdoutTransport::write(const folly::IOBuf& data) {
  return writeTo(file_, socket_, data);
}

folly::coro::Task<void> StdoutTransport::close() {
  file_.closeNoThrow();
  co_return;
}

} // namespace openmoq::moqx::logging
