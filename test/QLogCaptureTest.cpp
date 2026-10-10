/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "logging/QLogCapture.h"
#include "logging/CaptureQLogger.h"

#include <filesystem>
#include <set>
#include <string>

#include <folly/FileUtil.h>
#include <folly/Random.h>
#include <folly/json/json.h>
#include <gtest/gtest.h>

using openmoq::moqx::logging::CaptureQLogger;
using openmoq::moqx::logging::CcQLogger;
using openmoq::moqx::logging::QLogCapture;
using namespace std::chrono_literals;

namespace {

auto later(std::chrono::seconds s) {
  return std::chrono::steady_clock::now() + s;
}

} // namespace

TEST(QLogCapture, IdleByDefault) {
  QLogCapture capture;
  EXPECT_FALSE(capture.take().has_value());
  EXPECT_FALSE(capture.status().armed);
}

TEST(QLogCapture, TakesUpToCount) {
  QLogCapture capture;
  capture.arm(2, 60s, QLogCapture::Mode::Full, /*replace=*/false);
  EXPECT_EQ(capture.take(), QLogCapture::Mode::Full);
  EXPECT_EQ(capture.status().remaining, 1u);
  EXPECT_EQ(capture.take(), QLogCapture::Mode::Full);
  EXPECT_FALSE(capture.take().has_value());
  auto status = capture.status();
  EXPECT_FALSE(status.armed);
  EXPECT_EQ(status.captured, 2u);
}

TEST(QLogCapture, ExpiresAtDeadline) {
  QLogCapture capture;
  capture.arm(5, 10s, QLogCapture::Mode::Cc, /*replace=*/false);
  EXPECT_TRUE(capture.status(later(5s)).armed);
  EXPECT_FALSE(capture.status(later(11s)).armed);
  EXPECT_FALSE(capture.take(later(11s)).has_value());
  EXPECT_FALSE(capture.take().has_value());
}

TEST(QLogCapture, DisarmStops) {
  QLogCapture capture;
  capture.arm(5, 60s, QLogCapture::Mode::Cc, /*replace=*/false);
  EXPECT_TRUE(capture.take().has_value());
  capture.disarm();
  EXPECT_FALSE(capture.take().has_value());
  EXPECT_EQ(capture.status().captured, 1u);
}

TEST(QLogCapture, RearmReplaces) {
  QLogCapture capture;
  capture.arm(5, 60s, QLogCapture::Mode::Cc, /*replace=*/false);
  EXPECT_TRUE(capture.take().has_value());
  EXPECT_TRUE(capture.arm(1, 60s, QLogCapture::Mode::Full, /*replace=*/true));
  auto status = capture.status();
  EXPECT_EQ(status.remaining, 1u);
  EXPECT_EQ(status.captured, 0u);
  EXPECT_EQ(capture.take(), QLogCapture::Mode::Full);
}

TEST(QLogCapture, RefusesRearmWithoutReplace) {
  QLogCapture capture;
  EXPECT_TRUE(capture.arm(5, 60s, QLogCapture::Mode::Cc, /*replace=*/false));
  EXPECT_TRUE(capture.take().has_value());
  EXPECT_FALSE(capture.arm(1, 60s, QLogCapture::Mode::Full, /*replace=*/false));
  auto status = capture.status();
  EXPECT_EQ(status.mode, QLogCapture::Mode::Cc);
  EXPECT_EQ(status.remaining, 4u);
  EXPECT_EQ(status.captured, 1u);
}

TEST(QLogCapture, RearmsWithoutReplaceOnceDone) {
  QLogCapture expired;
  expired.arm(5, 10s, QLogCapture::Mode::Cc, /*replace=*/false);
  EXPECT_TRUE(expired.arm(1, 60s, QLogCapture::Mode::Cc, /*replace=*/false, later(11s)));

  QLogCapture exhausted;
  exhausted.arm(1, 60s, QLogCapture::Mode::Cc, /*replace=*/false);
  EXPECT_TRUE(exhausted.take().has_value());
  EXPECT_TRUE(exhausted.arm(1, 60s, QLogCapture::Mode::Cc, /*replace=*/false));

  QLogCapture disarmed;
  disarmed.arm(1, 60s, QLogCapture::Mode::Cc, /*replace=*/false);
  disarmed.disarm();
  EXPECT_TRUE(disarmed.arm(1, 60s, QLogCapture::Mode::Cc, /*replace=*/false));
}

TEST(QLogCapture, ClampsToMaxima) {
  QLogCapture capture;
  capture.arm(1000, 1h, QLogCapture::Mode::Cc, /*replace=*/false);
  EXPECT_EQ(capture.status().remaining, QLogCapture::kMaxCount);
  EXPECT_TRUE(capture.status(later(QLogCapture::kMaxDuration - 1s)).armed);
  EXPECT_FALSE(capture.status(later(QLogCapture::kMaxDuration + 1s)).armed);
}

TEST(QLogCapture, ParsesMode) {
  EXPECT_EQ(QLogCapture::parseMode("cc"), QLogCapture::Mode::Cc);
  EXPECT_EQ(QLogCapture::parseMode("full"), QLogCapture::Mode::Full);
  EXPECT_FALSE(QLogCapture::parseMode("all").has_value());
}

class CaptureQLoggerTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("moqx-qlog-test-" + std::to_string(folly::Random::rand64()));
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  // Logs packet, stream and recovery events; returns the event names written.
  template <typename Logger, typename... Args> std::set<std::string> logAndRead(Args&&... args) {
    const auto cid = quic::ConnectionId::createAndMaybeCrash({1, 2, 3, 4, 5, 6, 7, 8});
    {
      Logger logger(quic::VantagePoint::Server, std::forward<Args>(args)...);
      logger.setDcid(cid);
      logger.addPacketBuffered(quic::ProtectionType::KeyPhaseZero, 1200);
      logger.addDatagramReceived(1200);
      logger.addPacketDrop(1200, "test");
      logger.addStreamStateUpdate(4, "on headers", std::nullopt);
      logger.addMetricUpdate(10ms, 5ms, 8ms, 1ms);
    }
    std::string contents;
    EXPECT_TRUE(folly::readFile((dir_ / (cid.hex() + ".qlog")).c_str(), contents));
    const auto qlog = folly::parseJson(contents);
    std::set<std::string> names;
    for (const auto& event : qlog["traces"][0]["events"]) {
      names.insert(event["name"].asString());
    }
    return names;
  }

  std::filesystem::path dir_;
};

TEST_F(CaptureQLoggerTest, FullModeKeepsEverything) {
  auto names = logAndRead<CaptureQLogger>(dir_.string(), QLogCapture::Mode::Full);
  EXPECT_EQ(names.size(), 5u);
  EXPECT_TRUE(names.contains("quic:recovery_metrics_updated"));
}

TEST_F(CaptureQLoggerTest, CcModeDropsPacketAndStreamEvents) {
  auto names = logAndRead<CcQLogger>(dir_.string());
  EXPECT_EQ(names, std::set<std::string>{"quic:recovery_metrics_updated"});
}
