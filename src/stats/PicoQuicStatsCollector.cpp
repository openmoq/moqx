#include "stats/PicoQuicStatsCollector.h"

namespace openmoq::moqx::stats {

/* static */
std::shared_ptr<PicoQuicStatsCollector>
PicoQuicStatsCollector::create(std::shared_ptr<StatsRegistry> registry, folly::EventBase* evb) {
  auto collector = std::shared_ptr<PicoQuicStatsCollector>(new PicoQuicStatsCollector(evb));
  registry->registerCollector(collector);
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

void PicoQuicStatsCollector::onPathQualityDelta(const PathQualityDelta& d) {
  quicPacketsSent_ += d.packetsSent;
  quicPacketLoss_ += d.packetsLost;
  quicBytesWritten_ += d.bytesSent;
  quicBytesRead_ += d.bytesReceived;
  quicPacketRetransmissions_ += d.timerLosses + d.spuriousLosses;
  if (d.cwndBlocked) {
    ++quicCwndBlocked_;
  }
}

StatsSnapshot PicoQuicStatsCollector::snapshot() const {
  StatsSnapshot snap;

#define COPY_FIELD(type, name) snap.name = name##_;
  STATS_QUIC_COUNTER_FIELDS(COPY_FIELD)
  STATS_QUIC_GAUGE_FIELDS(COPY_FIELD)
#undef COPY_FIELD

  return snap;
}

folly::Executor* PicoQuicStatsCollector::owningExecutor() const {
  return evb_;
}

} // namespace openmoq::moqx::stats
