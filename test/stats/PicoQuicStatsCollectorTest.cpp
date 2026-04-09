/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "stats/PicoQuicStatsCollector.h"
#include "stats/StatsRegistry.h"

namespace openmoq::moqx::stats {

class PicoQuicStatsCollectorTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = std::make_shared<StatsRegistry>();
    collector_ = PicoQuicStatsCollector::create(registry_, &evb_);
  }

  // Dummy EventBase — not driven; only used as an identity for owningExecutor.
  folly::EventBase evb_;
  std::shared_ptr<StatsRegistry> registry_;
  std::shared_ptr<PicoQuicStatsCollector> collector_;
};

TEST_F(PicoQuicStatsCollectorTest, ConnectionLifecycle) {
  collector_->onConnectionCreated();
  collector_->onConnectionCreated();

  {
    auto snap = collector_->snapshot();
    EXPECT_EQ(snap.quicConnectionsCreated, 2);
    EXPECT_EQ(snap.quicConnectionsClosed, 0);
    EXPECT_EQ(snap.quicActiveConnections, 2);
  }

  collector_->onConnectionClosed();

  {
    auto snap = collector_->snapshot();
    EXPECT_EQ(snap.quicConnectionsCreated, 2);
    EXPECT_EQ(snap.quicConnectionsClosed, 1);
    EXPECT_EQ(snap.quicActiveConnections, 1);
  }
}

TEST_F(PicoQuicStatsCollectorTest, PathQualityDeltaAccumulation) {
  using Delta = moxygen::PicoQuicStatsCallback::PathQualityDelta;

  Delta d1{};
  d1.packetsSent = 10;
  d1.packetsLost = 1;
  d1.bytesSent = 1000;
  d1.bytesReceived = 800;
  d1.timerLosses = 1;
  d1.spuriousLosses = 0;
  d1.cwndBlocked = false;
  collector_->onPathQualityDelta(d1);

  Delta d2{};
  d2.packetsSent = 5;
  d2.packetsLost = 0;
  d2.bytesSent = 500;
  d2.bytesReceived = 400;
  d2.timerLosses = 0;
  d2.spuriousLosses = 1;
  d2.cwndBlocked = true;
  collector_->onPathQualityDelta(d2);

  auto snap = collector_->snapshot();
  EXPECT_EQ(snap.quicPacketsSent, 15);
  EXPECT_EQ(snap.quicPacketLoss, 1);
  EXPECT_EQ(snap.quicBytesWritten, 1500);
  EXPECT_EQ(snap.quicBytesRead, 1200);
  EXPECT_EQ(snap.quicPacketRetransmissions, 2); // timerLosses + spuriousLosses
  EXPECT_EQ(snap.quicCwndBlocked, 1);
}

TEST_F(PicoQuicStatsCollectorTest, CwndBlockedCountsEachEvent) {
  using Delta = moxygen::PicoQuicStatsCallback::PathQualityDelta;

  Delta d{};
  d.cwndBlocked = true;
  collector_->onPathQualityDelta(d);
  collector_->onPathQualityDelta(d);

  Delta notBlocked{};
  notBlocked.cwndBlocked = false;
  collector_->onPathQualityDelta(notBlocked);

  auto snap = collector_->snapshot();
  EXPECT_EQ(snap.quicCwndBlocked, 2);
}

TEST_F(PicoQuicStatsCollectorTest, StreamLifecycle) {
  collector_->onStreamCreated();
  collector_->onStreamCreated();
  collector_->onStreamClosed();
  collector_->onStreamReset();

  auto snap = collector_->snapshot();
  EXPECT_EQ(snap.quicStreamsCreated, 2);
  EXPECT_EQ(snap.quicStreamsClosed, 1);
  EXPECT_EQ(snap.quicStreamsReset, 1);
}

TEST_F(PicoQuicStatsCollectorTest, OwningExecutorIsEvb) {
  EXPECT_EQ(collector_->owningExecutor(), &evb_);
}

} // namespace openmoq::moqx::stats
