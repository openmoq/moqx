/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

// A sink that does not write lines, to test the AccessLogSink interface. It models a
// structured exporter such as OTLP: each record becomes a set of typed fields, and the
// batch goes out in requests that each succeed or fail as a whole.

#include <limits>
#include <map>
#include <mutex>

#include <folly/coro/BlockingWait.h>
#include <folly/logging/xlog.h>
#include <gtest/gtest.h>

#include "logging/access/MoqxAccessLog.h"

namespace openmoq::moqx::logging {
namespace {

using namespace std::chrono_literals;

// An AccessLogValue that owns its text, because a string_view value points into the record.
using Field = std::variant<
    std::monostate,
    uint64_t,
    bool,
    std::string,
    moxygen::AbsoluteLocation,
    std::chrono::system_clock::time_point>;
using Fields = std::map<std::string, Field, std::less<>>;

Field own(const AccessLogValue& value) {
  return std::visit(
      [](const auto& v) -> Field {
        if constexpr (std::is_same_v<std::decay_t<decltype(v)>, std::string_view>) {
          return std::string(v);
        } else {
          return v;
        }
      },
      value
  );
}

struct Exporter {
  std::mutex mutex;
  size_t maxPerRequest{std::numeric_limits<size_t>::max()};
  // Requests that fail before the next one succeeds.
  int failures{0};
  std::vector<std::vector<Fields>> requests;
  // The resource attributes that each request carried.
  std::vector<std::map<std::string, std::string>> resources;
  bool closed{false};

  std::vector<Fields> records() {
    std::lock_guard g(mutex);
    std::vector<Fields> all;
    for (const auto& request : requests) {
      all.insert(all.end(), request.begin(), request.end());
    }
    return all;
  }
};

class FieldSink : public AccessLogSink {
public:
  FieldSink(
      AccessLogSinkOptions options,
      std::shared_ptr<const AccessLogRelayInfo> relayInfo,
      std::vector<std::string_view> varNames,
      std::shared_ptr<Exporter> exporter
  )
      : AccessLogSink(std::move(options), std::move(relayInfo)), exporter_(std::move(exporter)) {
    for (auto name : varNames) {
      auto* var = findAccessLogVar(name);
      XCHECK(var) << name;
      vars_.push_back(var);
    }
  }

protected:
  void addRecord(const AccessLogRecord& record, const AccessLogRenderContext& ctx) override {
    Fields fields;
    for (const auto* var : vars_) {
      auto value = accessLogVarValue(*var, record, ctx);
      if (!std::holds_alternative<std::monostate>(value)) {
        fields.emplace(var->name, own(value));
      }
    }
    batch_.push_back(std::move(fields));
  }

  folly::coro::Task<AccessLogSinkWriteResult> writePending() override {
    std::lock_guard g(exporter_->mutex);
    if (exporter_->failures > 0) {
      exporter_->failures--;
      co_return AccessLogSinkWriteResult{AccessLogWriteStatus::Retry, 0ms};
    }
    auto count = std::min(batch_.size(), exporter_->maxPerRequest);
    exporter_->requests.emplace_back(batch_.begin(), batch_.begin() + count);
    auto resource = relayInfo().vars;
    resource.emplace("relay_id", relayInfo().relayId);
    exporter_->resources.push_back(std::move(resource));
    batch_.erase(batch_.begin(), batch_.begin() + count);
    co_return AccessLogSinkWriteResult{AccessLogWriteStatus::Ok, 0ms, count};
  }

  void dropOldest(size_t count) override { batch_.erase(batch_.begin(), batch_.begin() + count); }

  folly::coro::Task<void> closeOutput() override {
    batch_.clear();
    std::lock_guard g(exporter_->mutex);
    exporter_->closed = true;
    co_return;
  }

private:
  std::vector<const AccessLogVar*> vars_;
  std::shared_ptr<Exporter> exporter_;
  std::vector<Fields> batch_;
};

std::shared_ptr<const AccessLogRelayInfo> testRelayInfo() {
  static const auto relayInfo = std::make_shared<const AccessLogRelayInfo>(
      AccessLogRelayInfo{.relayId = "edge-1", .vars = {{"region", "us-east-1"}}}
  );
  return relayInfo;
}

std::unique_ptr<FieldSink>
fieldSink(std::shared_ptr<Exporter> exporter, size_t batchRecords = 256) {
  AccessLogSinkOptions opts;
  opts.name = "fields";
  opts.batchRecords = batchRecords;
  return std::make_unique<FieldSink>(
      std::move(opts),
      testRelayInfo(),
      std::vector<std::string_view>{"seq", "event", "session", "reason", "bytes", "dropped"},
      std::move(exporter)
  );
}

std::unique_ptr<AccessLogRecord> record(AccessLogEvent event, std::string session = "s1") {
  auto r = std::make_unique<AccessLogRecord>();
  r->event = event;
  r->time = std::chrono::system_clock::now();
  auto info = std::make_shared<AccessLogSessionInfo>();
  info->session = std::move(session);
  r->session = std::move(info);
  return r;
}

std::unique_ptr<MoqxAccessLog>
makeLog(std::unique_ptr<AccessLogSink> sink, size_t queueLimit = 1024) {
  std::vector<std::unique_ptr<AccessLogSink>> sinks;
  sinks.push_back(std::move(sink));
  return std::make_unique<MoqxAccessLog>(
      testRelayInfo(),
      std::move(sinks),
      allAccessLogSinks(1),
      queueLimit
  );
}

TEST(FieldSinkTest, RecordsArriveAsTypedFields) {
  auto exporter = std::make_shared<Exporter>();
  auto log = makeLog(fieldSink(exporter));
  log->start();
  auto gate = log->gateSession("live", "viewer", "s1");
  log->submit(record(AccessLogEvent::SETUP), log->eventMask(gate, AccessLogEvent::SETUP, false));
  auto subscribe = record(AccessLogEvent::SUBSCRIBE);
  subscribe->reason = "no such track";
  subscribe->bytes = 0;
  log->submit(std::move(subscribe), log->eventMask(gate, AccessLogEvent::SUBSCRIBE, false));
  log->stop(std::chrono::steady_clock::now() + 5s);

  auto records = exporter->records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(std::get<uint64_t>(records[0].at("seq")), 1u);
  EXPECT_EQ(std::get<std::string>(records[0].at("event")), "SETUP");
  EXPECT_EQ(std::get<std::string>(records[0].at("session")), "s1");
  EXPECT_FALSE(records[0].contains("bytes"));
  // The vars and the relay's identity go with each request, not with each record.
  ASSERT_EQ(exporter->resources.size(), exporter->requests.size());
  EXPECT_EQ(
      exporter->resources[0],
      (std::map<std::string, std::string>{{"region", "us-east-1"}, {"relay_id", "edge-1"}})
  );
  EXPECT_EQ(std::get<uint64_t>(records[1].at("seq")), 2u);
  // Text arrives without the escaping that a line needs, and a zero is kept.
  EXPECT_EQ(std::get<std::string>(records[1].at("reason")), "no such track");
  EXPECT_EQ(std::get<uint64_t>(records[1].at("bytes")), 0u);
  EXPECT_TRUE(exporter->closed);
}

TEST(FieldSinkTest, QueueDropsArriveAsALogDroppedRecord) {
  auto exporter = std::make_shared<Exporter>();
  auto log = makeLog(fieldSink(exporter), /*queueLimit=*/1);
  auto gate = log->gateSession("live", "viewer", "s1");
  auto mask = log->eventMask(gate, AccessLogEvent::SUBSCRIBE, false);
  for (int i = 0; i < 3; ++i) {
    log->submit(record(AccessLogEvent::SUBSCRIBE), mask);
  }
  log->start();
  log->stop(std::chrono::steady_clock::now() + 5s);

  auto records = exporter->records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(std::get<std::string>(records[0].at("event")), "LOG_DROPPED");
  EXPECT_EQ(std::get<uint64_t>(records[0].at("dropped")), 2u);
  EXPECT_FALSE(records[0].contains("session"));
}

TEST(FieldSinkTest, AFailedExportKeepsABoundedBacklogAndReportsTheLoss) {
  auto exporter = std::make_shared<Exporter>();
  exporter->failures = 1;
  auto sink = fieldSink(exporter, /*batchRecords=*/1);
  sink->append(*record(AccessLogEvent::SUBSCRIBE), 0);
  EXPECT_EQ(folly::coro::blockingWait(sink->flush()), AccessLogWriteStatus::Retry);
  // While the sink waits to retry, it keeps four records and drops the older ones.
  for (int i = 0; i < 10; ++i) {
    sink->append(*record(AccessLogEvent::SUBSCRIBE), 0);
  }
  folly::coro::blockingWait(sink->close(0));

  auto records = exporter->records();
  ASSERT_EQ(records.size(), 5u);
  EXPECT_EQ(std::get<uint64_t>(records[0].at("seq")), 8u);
  EXPECT_EQ(std::get<std::string>(records[4].at("event")), "LOG_DROPPED");
  EXPECT_EQ(std::get<uint64_t>(records[4].at("dropped")), 7u);
  EXPECT_EQ(sink->stats().dropped, 7u);
  EXPECT_EQ(sink->stats().records, 5u);
}

TEST(FieldSinkTest, ARequestLimitSendsTheRestWithoutErrors) {
  auto exporter = std::make_shared<Exporter>();
  exporter->maxPerRequest = 2;
  auto sink = fieldSink(exporter);
  for (int i = 0; i < 5; ++i) {
    sink->append(*record(AccessLogEvent::SUBSCRIBE), 0);
  }
  EXPECT_EQ(folly::coro::blockingWait(sink->flush()), AccessLogWriteStatus::Ok);
  // Records are left, so the sink is due again at once.
  EXPECT_TRUE(sink->hasPending());
  EXPECT_TRUE(sink->retrying());
  EXPECT_LE(sink->retryAt(), std::chrono::steady_clock::now());
  folly::coro::blockingWait(sink->close(0));

  EXPECT_EQ(exporter->requests.size(), 3u);
  EXPECT_EQ(exporter->records().size(), 5u);
  EXPECT_EQ(sink->stats().records, 5u);
  EXPECT_EQ(sink->stats().writeErrors, 0u);
}

} // namespace
} // namespace openmoq::moqx::logging
