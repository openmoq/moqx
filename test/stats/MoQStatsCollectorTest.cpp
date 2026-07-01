/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <string>

#include "stats/MoQStatsCollector.h"
#include "stats/StatsRegistry.h"

namespace openmoq::moqx::stats {

namespace {
std::string prometheusText(const StatsSnapshot& snap) {
  auto out = StatsSnapshot::formatPrometheus(snap);
  out->coalesce();
  return std::string(reinterpret_cast<const char*>(out->data()), out->length());
}
} // namespace

class MoQStatsCollectorTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = std::make_shared<StatsRegistry>();
    collector_ = MoQStatsCollector::create_moq_stats_collector(registry_);
  }

  std::shared_ptr<StatsRegistry> registry_;
  std::shared_ptr<MoQStatsCollector> collector_;
};

TEST_F(MoQStatsCollectorTest, SubgroupResetBrokenDownByReason) {
  auto pub = collector_->publisherCallback();
  // Two delivery-timeout resets, one cancelled; metric #2 is just one label.
  pub->onSubgroupReset(moxygen::ResetStreamErrorCode::DELIVERY_TIMEOUT);
  pub->onSubgroupReset(moxygen::ResetStreamErrorCode::DELIVERY_TIMEOUT);
  pub->onSubgroupReset(moxygen::ResetStreamErrorCode::CANCELLED);

  auto sub = collector_->subscriberCallback();
  sub->onSubgroupReset(moxygen::ResetStreamErrorCode::INTERNAL_ERROR);

  auto snap = collector_->snapshot();
  const auto timeoutIdx =
      resetStreamErrorCodeIndex(moxygen::ResetStreamErrorCode::DELIVERY_TIMEOUT);
  const auto cancelledIdx = resetStreamErrorCodeIndex(moxygen::ResetStreamErrorCode::CANCELLED);
  const auto internalIdx = resetStreamErrorCodeIndex(moxygen::ResetStreamErrorCode::INTERNAL_ERROR);

  EXPECT_EQ(snap.pubSubgroupResetByResetCodes[timeoutIdx], 2);
  EXPECT_EQ(snap.pubSubgroupResetByResetCodes[cancelledIdx], 1);
  EXPECT_EQ(snap.pubSubgroupResetByResetCodes[internalIdx], 0);
  EXPECT_EQ(snap.subSubgroupResetByResetCodes[internalIdx], 1);
  EXPECT_EQ(snap.subSubgroupResetByResetCodes[timeoutIdx], 0);
}

TEST_F(MoQStatsCollectorTest, UnknownResetCodeFallsIntoUnknownSlot) {
  auto pub = collector_->publisherCallback();
  // 0x99 is not a defined ResetStreamErrorCode; it must map to the last slot.
  pub->onSubgroupReset(static_cast<moxygen::ResetStreamErrorCode>(0x99));

  auto snap = collector_->snapshot();
  EXPECT_EQ(snap.pubSubgroupResetByResetCodes[kResetStreamErrorCodeCount - 1], 1);
  EXPECT_EQ(kResetStreamErrorCodeLabels[kResetStreamErrorCodeCount - 1], "unknown");
}

TEST_F(MoQStatsCollectorTest, ObjectAckLatencyHistogram) {
  auto pub = collector_->publisherCallback();
  pub->recordObjectAckLatency(120);
  pub->recordObjectAckLatency(880);

  auto snap = collector_->snapshot();
  EXPECT_EQ(snap.moqObjectAckLatencyCount, 2);
  EXPECT_EQ(snap.moqObjectAckLatencySum, 1000);
}

TEST_F(MoQStatsCollectorTest, PrometheusExportsResetAndAckLatency) {
  auto pub = collector_->publisherCallback();
  pub->onSubgroupReset(moxygen::ResetStreamErrorCode::DELIVERY_TIMEOUT);
  pub->recordObjectAckLatency(300);

  auto text = prometheusText(collector_->snapshot());

  EXPECT_NE(
      text.find("moqx_pubSubgroupReset_total{code=\"delivery_timeout\"} 1"),
      std::string::npos
  );
  // Total is sum() over labels: an unhit reason still emits a zero series.
  EXPECT_NE(text.find("moqx_pubSubgroupReset_total{code=\"cancelled\"} 0"), std::string::npos);
  EXPECT_NE(text.find("moqx_moqObjectAckLatency_microseconds_count 1"), std::string::npos);
}

} // namespace openmoq::moqx::stats
