#include "o_rly/stats/QuicStatsCollector.h"

#include <folly/io/async/EventBaseManager.h>
#include <glog/logging.h>

namespace openmoq::o_rly::stats {

// Private inner class: the quic::QuicTransportStatsCallback adapter owned by mvfst.
// Captures the QUIC worker's EventBase on first callback, then registers the collector
// with the registry. Forwards transport events to the QuicStatsCollector data object.

class QuicStatsCollector::Callback : public quic::QuicTransportStatsCallback {
public:
  explicit Callback(std::weak_ptr<StatsRegistry> registry)
      : data_(std::shared_ptr<QuicStatsCollector>(new QuicStatsCollector(registry))),
        registry_(std::move(registry)) {}

  ~Callback() override {
    if (auto reg = registry_.lock()) {
      reg->deregisterCollector(data_.get());
    }
  }

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
  void onNewConnection() override { ++data_->quicConnectionsCreated_; }
  void onConnectionClose(quic::Optional<quic::QuicErrorCode>) override {
    ++data_->quicConnectionsClosed_;
  }

  // Untracked no-op callbacks
  void onRxDelaySample(uint64_t) override {}
  void onDuplicatedPacketReceived() override {}
  void onOutOfOrderPacketReceived() override {}
  void onPacketProcessed() override {}
  void onPacketRetransmission() override {}
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
  void onNewQuicStream() override {}
  void onQuicStreamClosed() override {}
  void onQuicStreamReset(quic::QuicErrorCode) override {}
  void onConnFlowControlUpdate() override {}
  void onConnFlowControlBlocked() override {}
  void onStatelessReset() override {}
  void onStreamFlowControlUpdate() override {}
  void onStreamFlowControlBlocked() override {}
  void onCwndBlocked() override {}
  void onInflightBytesSample(uint64_t) override {}
  void onRttSample(uint64_t) override {}
  void onBandwidthSample(uint64_t) override {}
  void onCwndHintBytesSample(uint64_t) override {}
  void onNewCongestionController(quic::CongestionControlType) override {}
  void onPTO() override {}
  void onRead(size_t) override {}
  void onWrite(size_t) override {}
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
  void onDatagramDroppedOnWrite() override {}
  void onDatagramDroppedOnRead() override {}
  void onShortHeaderPadding(size_t) override {}
  void onPacerTimerLagged() override {}
  void onPeerMaxUniStreamsLimitSaturated() override {}
  void onPeerMaxBidiStreamsLimitSaturated() override {}
  void onConnectionIdCreated(size_t) override {}
  void onKeyUpdateAttemptInitiated() override {}
  void onKeyUpdateAttemptReceived() override {}
  void onKeyUpdateAttemptSucceeded() override {}

private:
  // Lazily captures the QUIC worker's EventBase on the first callback and
  // registers data_ with the StatsRegistry.  Idempotent.
  void captureWorkerEvbIfNeeded() {
    // Fast path: already captured.  All writes are on the worker EventBase so
    // a relaxed load is sufficient — we're on the writer thread.
    if (data_->owningEvb_.load(std::memory_order_relaxed) != nullptr) {
      return;
    }
    auto* evb = folly::EventBaseManager::get()->getExistingEventBase();
    CHECK(evb) << "QuicStatsCollector::Callback: first callback not on an EventBase thread";
    data_->owningEvb_.store(evb, std::memory_order_relaxed);

    if (auto reg = registry_.lock()) {
      reg->registerCollector(data_);
    }
  }

  std::shared_ptr<QuicStatsCollector> data_;
  std::weak_ptr<StatsRegistry> registry_;
};

QuicStatsCollector::Factory::Factory(std::shared_ptr<StatsRegistry> registry)
    : registry_(std::move(registry)) {}

std::unique_ptr<quic::QuicTransportStatsCallback> QuicStatsCollector::Factory::make() {
  return std::make_unique<Callback>(registry_);
}

QuicStatsCollector::QuicStatsCollector(std::weak_ptr<StatsRegistry> registry)
    : registry_(std::move(registry)) {}

QuicStatsCollector::~QuicStatsCollector() {
  if (auto reg = registry_.lock()) {
    reg->deregisterCollector(this);
  }
}

StatsSnapshot QuicStatsCollector::snapshot() const {
  // Called on owningExecutor_ (QUIC IO thread)
  StatsSnapshot snap;

#define COPY_FIELD(type, name) snap.name = name##_;
  STATS_QUIC_COUNTER_FIELDS(COPY_FIELD)
#undef COPY_FIELD

  return snap;
}

folly::Executor* QuicStatsCollector::owningExecutor() const {
  auto* evb = owningEvb_.load(std::memory_order_relaxed);
  DCHECK(evb) << "owningExecutor() called before worker EventBase was captured";
  return evb;
}

} // namespace openmoq::o_rly::stats
