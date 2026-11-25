/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <string>

#include <folly/Expected.h>
#include <folly/File.h>

#include "logging/access/AccessLogTransport.h"

namespace openmoq::moqx::logging {

// Appends lines to a file. reopen() reopens the path, for logrotate.
class FileTransport : public AccessLogTransport {
public:
  static folly::Expected<std::unique_ptr<FileTransport>, std::string> open(std::string path);
  // Writes to fd 1, which the transport does not own or reopen.
  static std::unique_ptr<FileTransport> standardOutput();

  folly::coro::Task<AccessLogWriteResult> write(const folly::IOBuf& data) override;
  void reopen() override;
  folly::coro::Task<void> close() override;

private:
  FileTransport(std::string path, folly::File file)
      : path_(std::move(path)), file_(std::move(file)) {}

  std::string path_;
  folly::File file_;
};

} // namespace openmoq::moqx::logging
