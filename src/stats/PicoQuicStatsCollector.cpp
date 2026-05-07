/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "stats/PicoQuicStatsCollector.h"

#include <folly/tracing/StaticTracepoint.h>

namespace openmoq::moqx::stats {

/* static */
std::shared_ptr<PicoQuicStatsCollector> PicoQuicStatsCollector::create(
    std::shared_ptr<StatsRegistry> registry,
    folly::EventBase* evb,
    EventBaseStatsCollector* evbCollector
) {
  auto collector = std::shared_ptr<PicoQuicStatsCollector>(new PicoQuicStatsCollector(evb));
  registry->registerCollector(collector);
  if (evbCollector) {
    evbCollector->addLoopObserver([c = collector.get()](int64_t busyUs, int64_t /*idleUs*/) {
      uint64_t sent = c->quicPacketsSent_;
      uint64_t recv = c->quicPacketsReceived_;
      uint64_t dSent = sent - c->prevLoopPktsSent_;
      uint64_t dRecv = recv - c->prevLoopPktsRecv_;
      c->evbPktsSentPerLoop_.addValue(dSent);
      c->evbPktsRecvPerLoop_.addValue(dRecv);
      c->prevLoopPktsSent_ = sent;
      c->prevLoopPktsRecv_ = recv;
      FOLLY_SDT(moqx, evb_loop_sample, busyUs, dSent, dRecv);
    });
  }
  return collector;
}

PicoQuicStatsCollector::PicoQuicStatsCollector(folly::EventBase* evb) : evb_(evb) {}

void PicoQuicStatsCollector::onConnectionCreated() {
  ++quicConnectionsCreated_;
  ++quicActiveConnections_;
}

void PicoQuicStatsCollector::onConnectionClosed() {
  ++quicConnectionsClosed_;
  --quicActiveConnections_;
}

void PicoQuicStatsCollector::onStreamCreated() {
  ++quicStreamsCreated_;
}

void PicoQuicStatsCollector::onStreamClosed() {
  ++quicStreamsClosed_;
}

void PicoQuicStatsCollector::onStreamReset() {
  ++quicStreamsReset_;
}

void PicoQuicStatsCollector::onPacketsSent(uint64_t n) {
  quicPacketsSent_ += n;
}

void PicoQuicStatsCollector::onPacketsReceived(uint64_t n) {
  quicPacketsReceived_ += n;
}

void PicoQuicStatsCollector::onPathQualityDelta(const PathQualityDelta& d) {
  quicPacketLoss_ += d.packetsLost;
  quicBytesWritten_ += d.bytesSent;
  quicBytesRead_ += d.bytesReceived;
  quicPacketRetransmissions_ += d.timerLosses;
  quicPTO_ += d.timerLosses;
  quicPacketSpuriousLoss_ += d.spuriousLosses;
  if (d.cwndBlocked) {
    ++quicCwndBlocked_;
  }
  if (d.smoothedRttUs > 0) {
    quicRttSample_.addValue(d.smoothedRttUs / 1000);
  }
  if (d.receiveBytesPerSec > 0) {
    quicBandwidthSample_.addValue(d.receiveBytesPerSec * 8);
  }
  if (d.bytesInTransit > 0) {
    quicInflightBytesSample_.addValue(d.bytesInTransit);
  }
}

StatsSnapshot PicoQuicStatsCollector::snapshot() const {
  StatsSnapshot snap;

#define COPY_FIELD(type, name) snap.name = name##_;
  STATS_QUIC_COUNTER_FIELDS(COPY_FIELD)
  STATS_QUIC_GAUGE_FIELDS(COPY_FIELD)
#undef COPY_FIELD

#define COPY_HISTOGRAM(name, bounds, unit)                                                         \
  name##_.fillCumulative(snap.name##Buckets);                                                      \
  snap.name##Sum = name##_.sum;                                                                    \
  snap.name##Count = name##_.count;
  STATS_QUIC_HISTOGRAM_FIELDS(COPY_HISTOGRAM)
#undef COPY_HISTOGRAM

  return snap;
}

folly::Executor* PicoQuicStatsCollector::owningExecutor() const {
  return evb_;
}

} // namespace openmoq::moqx::stats
