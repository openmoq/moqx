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
  capture.arm(2, 60s, QLogCapture::Mode::Full);
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
  capture.arm(5, 10s, QLogCapture::Mode::Cc);
  EXPECT_TRUE(capture.status(later(5s)).armed);
  EXPECT_FALSE(capture.status(later(11s)).armed);
  EXPECT_FALSE(capture.take(later(11s)).has_value());
  EXPECT_FALSE(capture.take().has_value());
}

TEST(QLogCapture, DisarmStops) {
  QLogCapture capture;
  capture.arm(5, 60s, QLogCapture::Mode::Cc);
  EXPECT_TRUE(capture.take().has_value());
  capture.disarm();
  EXPECT_FALSE(capture.take().has_value());
  EXPECT_EQ(capture.status().captured, 1u);
}

TEST(QLogCapture, RearmReplaces) {
  QLogCapture capture;
  capture.arm(5, 60s, QLogCapture::Mode::Cc);
  EXPECT_TRUE(capture.take().has_value());
  capture.arm(1, 60s, QLogCapture::Mode::Full);
  auto status = capture.status();
  EXPECT_EQ(status.remaining, 1u);
  EXPECT_EQ(status.captured, 0u);
  EXPECT_EQ(capture.take(), QLogCapture::Mode::Full);
}

TEST(QLogCapture, ClampsToMaxima) {
  QLogCapture capture;
  capture.arm(1000, 1h, QLogCapture::Mode::Cc);
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

  // Logs one stream event and one recovery event, returns the event names written.
  std::set<std::string> logAndRead(bool ccOnly) {
    const auto cid = quic::ConnectionId::createAndMaybeCrash({1, 2, 3, 4, 5, 6, 7, 8});
    {
      CaptureQLogger logger(quic::VantagePoint::Server, dir_.string(), ccOnly);
      logger.setDcid(cid);
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

TEST_F(CaptureQLoggerTest, FullKeepsStreamEvents) {
  auto names = logAndRead(/*ccOnly=*/false);
  EXPECT_EQ(names.size(), 2u);
  EXPECT_TRUE(names.contains("quic:recovery_metrics_updated"));
}

TEST_F(CaptureQLoggerTest, CcOnlyDropsStreamEvents) {
  auto names = logAndRead(/*ccOnly=*/true);
  EXPECT_EQ(names, std::set<std::string>{"quic:recovery_metrics_updated"});
}
