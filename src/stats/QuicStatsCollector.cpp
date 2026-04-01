#include "moqx/stats/QuicStatsCollector.h"

#include <folly/io/async/EventBaseManager.h>
#include <glog/logging.h>

namespace openmoq::moqx::stats {

// Private inner class: the quic::QuicTransportStatsCallback adapter owned by mvfst.
// Register eagerly at construction The evb is captured lazily on first packet.
// Forwards transport events to the QuicStatsCollector data object.

class QuicStatsCollector::Callback : public quic::QuicTransportStatsCallback {
public:
  explicit Callback(std::weak_ptr<StatsRegistry> registry)
      : data_(std::shared_ptr<QuicStatsCollector>(new QuicStatsCollector())) {
    if (auto reg = registry.lock()) {
      reg->registerCollector(data_);
    }
  }

  ~Callback() override = default;

  // --- Tracked callbacks ---
  // Based on QuicServerWorker, the first ones to trigger can be onPacketReceived or
  // onPacketDropped, so we capture the EventBase in these callbacks.
  void onPacketReceived() override {
    captureWorkerEvbIfNeeded();
    ++data_->quicPacketsReceived_;
  }
  void onPacketSent() override { ++data_->quicPacketsSent_; }
  void onPacketDropped(quic::PacketDropReason) override {
    captureWorkerEvbIfNeeded();
    ++data_->quicPacketsDropped_;
  }
  void onPacketLoss() override { ++data_->quicPacketLoss_; }
  void onPacketRetransmission() override { ++data_->quicPacketRetransmissions_; }
  void onNewConnection() override {
    ++data_->quicConnectionsCreated_;
    ++data_->quicActiveConnections_;
  }
  void onConnectionClose(quic::Optional<quic::QuicErrorCode>) override {
    ++data_->quicConnectionsClosed_;
    --data_->quicActiveConnections_;
  }
  void onNewQuicStream() override {
    ++data_->quicStreamsCreated_;
    ++data_->quicActiveStreams_;
  }
  void onQuicStreamClosed() override {
    ++data_->quicStreamsClosed_;
    --data_->quicActiveStreams_;
  }
  void onQuicStreamReset(quic::QuicErrorCode) override {
    ++data_->quicStreamsReset_;
    --data_->quicActiveStreams_;
  }
  void onConnFlowControlBlocked() override { ++data_->quicConnFlowControlBlocked_; }
  void onStreamFlowControlBlocked() override { ++data_->quicStreamFlowControlBlocked_; }
  void onCwndBlocked() override { ++data_->quicCwndBlocked_; }
  void onRead(size_t bytes) override { data_->quicBytesRead_ += bytes; }
  void onWrite(size_t bytes) override { data_->quicBytesWritten_ += bytes; }
  void onDatagramDroppedOnWrite() override { ++data_->quicDatagramsDroppedOnWrite_; }
  void onDatagramDroppedOnRead() override { ++data_->quicDatagramsDroppedOnRead_; }
  void onPeerMaxUniStreamsLimitSaturated() override {
    ++data_->quicPeerMaxUniStreamsLimitSaturated_;
  }
  void onPeerMaxBidiStreamsLimitSaturated() override {
    ++data_->quicPeerMaxBidiStreamsLimitSaturated_;
  }

  // --- Untracked callbacks (no-op) ---
  void onRxDelaySample(uint64_t) override {}
  void onDuplicatedPacketReceived() override {}
  void onOutOfOrderPacketReceived() override {}
  void onPacketProcessed() override {}
  void onPacketSpuriousLoss() override {}
  void onPersistentCongestion() override {}
  void onPacketForwarded() override {}
  void onPacketDroppedByEgressPolicer() override {}
  void onForwardedPacketReceived() override {}
  void onForwardedPacketProcessed() override {}
  void onClientInitialReceived(quic::QuicVersion) override {}
  void onConnectionRateLimited() override {}
  void onConnectionWritableBytesLimited() override {}
  void onNewTokenReceived() override {}
  void onNewTokenIssued() override {}
  void onTokenDecryptFailure() override {}
  void onConnectionCloseZeroBytesWritten() override {}
  void onConnectionMigration() override {}
  void onPathAdded() override {}
  void onPathValidationSuccess() override {}
  void onPathValidationFailure() override {}
  void onConnFlowControlUpdate() override {}
  void onStatelessReset() override {}
  void onStreamFlowControlUpdate() override {}
  void onInflightBytesSample(uint64_t) override {}
  void onRttSample(uint64_t) override {}
  void onBandwidthSample(uint64_t) override {}
  void onCwndHintBytesSample(uint64_t) override {}
  void onCongestionControllerResumed() override {}
  void onNewCongestionController(quic::CongestionControlType) override {}
  void onPTO() override {}
  void onUDPSocketWriteError(SocketErrorType) override {}
  void onTransportKnobApplied(quic::TransportKnobParamId) override {}
  void onTransportKnobError(quic::TransportKnobParamId) override {}
  void onTransportKnobOutOfOrder(quic::TransportKnobParamId) override {}
  void onServerUnfinishedHandshake() override {}
  void onZeroRttBuffered() override {}
  void onZeroRttBufferedPruned() override {}
  void onZeroRttAccepted() override {}
  void onZeroRttRejected() override {}
  void onZeroRttPrimingAccepted() override {}
  void onZeroRttPrimingRejected() override {}
  void onDatagramRead(size_t) override {}
  void onDatagramWrite(size_t) override {}
  void onShortHeaderPadding(size_t) override {}
  void onPacerTimerLagged() override {}
  void onConnectionIdCreated(size_t) override {}
  void onKeyUpdateAttemptInitiated() override {}
  void onKeyUpdateAttemptReceived() override {}
  void onKeyUpdateAttemptSucceeded() override {}

private:
  // Lazily captures the QUIC worker's EventBase on first packet.
  void captureWorkerEvbIfNeeded() {
    // Fast path: already captured.  All writes are on the worker EventBase so
    // a relaxed load is sufficient — we're on the writer thread.
    if (data_->owningEvb_.load(std::memory_order_relaxed) != nullptr) {
      return;
    }
    auto* evb = folly::EventBaseManager::get()->getExistingEventBase();
    CHECK(evb) << "QuicStatsCollector::Callback: first callback not on an EventBase thread";
    data_->owningEvb_.store(evb, std::memory_order_relaxed);
  }

  std::shared_ptr<QuicStatsCollector> data_;
};

QuicStatsCollector::Factory::Factory(std::shared_ptr<StatsRegistry> registry)
    : registry_(std::move(registry)) {}

std::unique_ptr<quic::QuicTransportStatsCallback> QuicStatsCollector::Factory::make() {
  return std::make_unique<Callback>(registry_);
}

StatsSnapshot QuicStatsCollector::snapshot() const {
  // Called on owningExecutor_ (QUIC IO thread)
  StatsSnapshot snap;

#define COPY_FIELD(type, name) snap.name = name##_;
  STATS_QUIC_COUNTER_FIELDS(COPY_FIELD)
  STATS_QUIC_GAUGE_FIELDS(COPY_FIELD)
#undef COPY_FIELD

  return snap;
}

folly::Executor* QuicStatsCollector::owningExecutor() const {
  return owningEvb_.load(std::memory_order_relaxed);
}

} // namespace openmoq::moqx::stats
