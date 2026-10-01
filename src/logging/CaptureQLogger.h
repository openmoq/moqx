/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>

#include <folly/logging/xlog.h>
#include <quic/logging/FileQLogger.h>

namespace openmoq::moqx::logging {

/**
 * Streaming FileQLogger for an on-demand capture. With ccOnly, per-packet and
 * per-stream events are dropped: congestion control, RTT, loss and pacing
 * events remain, at a fraction of the serialization cost on the IO thread.
 */
class CaptureQLogger : public quic::FileQLogger {
public:
  CaptureQLogger(quic::VantagePoint vantagePoint, std::string dir, bool ccOnly)
      : quic::FileQLogger(
            vantagePoint,
            "MOQT",
            std::move(dir),
            /*prettyJson=*/false,
            /*streaming=*/true,
            /*compress=*/false
        ),
        ccOnly_(ccOnly) {}

  void setDcid(quic::Optional<quic::ConnectionId> connID) override {
    if (connID.has_value()) {
      XLOG(INFO) << "qlog capture: connection " << connID->hex() << " ("
                 << (ccOnly_ ? "cc" : "full") << ")";
    }
    quic::FileQLogger::setDcid(std::move(connID));
  }

  void addPacket(const quic::RegularQuicPacket& packet, uint64_t size) override {
    if (!ccOnly_) {
      quic::FileQLogger::addPacket(packet, size);
    }
  }
  void addPacket(const quic::VersionNegotiationPacket& packet, uint64_t size, bool recvd) override {
    if (!ccOnly_) {
      quic::FileQLogger::addPacket(packet, size, recvd);
    }
  }
  void addPacket(const quic::RegularQuicWritePacket& packet, uint64_t size) override {
    if (!ccOnly_) {
      quic::FileQLogger::addPacket(packet, size);
    }
  }
  void addPacket(const quic::RetryPacket& packet, uint64_t size, bool recvd) override {
    if (!ccOnly_) {
      quic::FileQLogger::addPacket(packet, size, recvd);
    }
  }
  void addPacketBuffered(quic::ProtectionType type, uint64_t size) override {
    if (!ccOnly_) {
      quic::FileQLogger::addPacketBuffered(type, size);
    }
  }
  void addDatagramReceived(uint64_t len) override {
    if (!ccOnly_) {
      quic::FileQLogger::addDatagramReceived(len);
    }
  }
  void addStreamStateUpdate(
      quic::StreamId id,
      std::string update,
      quic::Optional<std::chrono::milliseconds> sinceCreation
  ) override {
    if (!ccOnly_) {
      quic::FileQLogger::addStreamStateUpdate(id, std::move(update), sinceCreation);
    }
  }
  void
  addPriorityUpdate(quic::StreamId id, quic::PriorityQueue::PriorityLogFields fields) override {
    if (!ccOnly_) {
      quic::FileQLogger::addPriorityUpdate(id, std::move(fields));
    }
  }

private:
  const bool ccOnly_;
};

} // namespace openmoq::moqx::logging
