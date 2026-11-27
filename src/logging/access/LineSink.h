/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include <folly/io/IOBufQueue.h>
#include <quic/common/CircularDeque.h>

#include "logging/access/AccessLogSink.h"
#include "logging/access/AccessLogTransport.h"
#include "logging/access/TemplateEncoder.h"

namespace openmoq::moqx::logging {

// A sink that encodes each record as one line of text and writes the lines to a transport.
class LineSink : public AccessLogSink {
public:
  LineSink(
      AccessLogSinkOptions options,
      TemplateEncoder encoder,
      std::unique_ptr<AccessLogTransport> transport
  );

  void reopen() override;

protected:
  void addRecord(const AccessLogRecord& record, const AccessLogRenderContext& ctx) override;
  folly::coro::Task<AccessLogSinkWriteResult> writePending() override;
  void dropOldest(size_t count) override;
  folly::coro::Task<void> closeOutput() override;

private:
  void clearPending();
  // Removes the bytes a partial write sent, and returns the lines it completed.
  size_t consume(size_t bytes);

  TemplateEncoder encoder_;
  std::unique_ptr<AccessLogTransport> transport_;
  std::string scratch_;
  // The lines waiting to be written, with their newlines.
  folly::IOBufQueue pendingLines_;
  // The length of each pending line. Excludes any bytes that a partial write already sent.
  quic::CircularDeque<size_t> lengths_;
  // lengths_ copied into contiguous memory for the transport.
  std::vector<size_t> lineLengths_;
  bool firstLinePartial_{false};
  bool reopenPending_{false};
};

} // namespace openmoq::moqx::logging
