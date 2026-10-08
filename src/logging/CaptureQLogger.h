/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

#include <folly/logging/xlog.h>
#include <quic/logging/FileQLogger.h>

#include "logging/QLogCapture.h"

namespace openmoq::moqx::logging {

/**
 * Streaming FileQLogger for a captured connection. Logs the connection ID,
 * which names the qlog file, at INFO once mvfst sets it.
 */
class CaptureQLogger : public quic::FileQLogger {
public:
  CaptureQLogger(quic::VantagePoint vantagePoint, std::string dir, QLogCapture::Mode mode)
      : quic::FileQLogger(
            vantagePoint,
            "MOQT",
            std::move(dir),
            /*prettyJson=*/false,
            /*streaming=*/true,
            /*compress=*/false
        ),
        mode_(mode) {}

  void setDcid(quic::Optional<quic::ConnectionId> connID) override {
    if (connID.has_value()) {
      XLOG(INFO) << "qlog capture: connection " << connID->hex() << " ("
                 << QLogCapture::modeName(mode_) << ")";
    }
    quic::FileQLogger::setDcid(std::move(connID));
  }

private:
  QLogCapture::Mode mode_;
};

/**
 * CaptureQLogger that drops per-packet and per-stream events. Congestion
 * control, RTT, loss and pacing events remain, at a fraction of the
 * serialization cost on the IO thread.
 */
class CcQLogger : public CaptureQLogger {
public:
  CcQLogger(quic::VantagePoint vantagePoint, std::string dir)
      : CaptureQLogger(vantagePoint, std::move(dir), QLogCapture::Mode::Cc) {}

  void addPacket(const quic::RegularQuicPacket&, uint64_t) override {}
  void addPacket(const quic::VersionNegotiationPacket&, uint64_t, bool) override {}
  void addPacket(const quic::RegularQuicWritePacket&, uint64_t) override {}
  void addPacket(const quic::RetryPacket&, uint64_t, bool) override {}
  void addPacketBuffered(quic::ProtectionType, uint64_t) override {}
  void addDatagramReceived(uint64_t) override {}
  void addStreamStateUpdate(quic::StreamId, std::string, quic::Optional<std::chrono::milliseconds>)
      override {}
  void addPriorityUpdate(quic::StreamId, quic::PriorityQueue::PriorityLogFields) override {}
};

} // namespace openmoq::moqx::logging
