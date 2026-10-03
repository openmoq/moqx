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

#include <quic/logging/FileQLogger.h>

namespace openmoq::moqx::logging {

/**
 * Streaming FileQLogger that drops per-packet and per-stream events. Congestion
 * control, RTT, loss and pacing events remain, at a fraction of the
 * serialization cost on the IO thread.
 */
class CcQLogger : public quic::FileQLogger {
public:
  CcQLogger(quic::VantagePoint vantagePoint, std::string dir)
      : quic::FileQLogger(
            vantagePoint,
            "MOQT",
            std::move(dir),
            /*prettyJson=*/false,
            /*streaming=*/true,
            /*compress=*/false
        ) {}

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
