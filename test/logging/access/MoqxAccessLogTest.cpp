/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "logging/access/MoqxAccessLog.h"
#include "logging/access/AccessLogSetup.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <folly/ScopeGuard.h>
#include <folly/String.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Sleep.h>
#include <folly/coro/Timeout.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/synchronization/Baton.h>
#include <gtest/gtest.h>

#include "logging/access/FileTransport.h"
#include "stats/AccessLogPrometheus.h"

namespace openmoq::moqx::logging {
namespace {

using namespace std::chrono_literals;

std::vector<std::string> splitLines(const folly::IOBuf& data) {
  auto text = data.to<std::string>();
  std::vector<std::string> lines;
  folly::split('\n', text, lines);
  if (!lines.empty() && lines.back().empty()) {
    lines.pop_back();
  }
  return lines;
}

// Hands each write to a function.
class CallbackTransport : public AccessLogTransport {
public:
  using Callback = std::function<void(const folly::IOBuf&)>;
  explicit CallbackTransport(Callback cb) : cb_(std::move(cb)) {}

  folly::coro::Task<AccessLogWriteResult> write(const folly::IOBuf& data) override {
    cb_(data);
    co_return AccessLogWriteResult{};
  }
  folly::coro::Task<void> close() override { co_return; }

private:
  Callback cb_;
};

struct Captured {
  std::mutex mutex;
  std::vector<std::string> lines;

  std::vector<std::string> get() {
    std::lock_guard g(mutex);
    return lines;
  }
};

AccessLogSinkSelection allEvents(size_t sink) {
  AccessLogSinkSelection selection;
  selection.sink = sink;
  selection.events.fill(true);
  return selection;
}

TemplateEncoder encoderFor(std::string_view tmpl) {
  TemplateEncoder encoder;
  for (auto cls : {AccessLogClass::Session, AccessLogClass::Request, AccessLogClass::Internal}) {
    encoder.setClassTemplate(cls, *AccessLogTemplate::parse(tmpl));
  }
  encoder.setEventTemplate(
      AccessLogEvent::LOG_DROPPED,
      *AccessLogTemplate::parse("$seq $event $dropped")
  );
  return encoder;
}

std::unique_ptr<AccessLogSink>
callbackSink(std::shared_ptr<Captured> captured, std::string_view tmpl = "$seq $event $session") {
  AccessLogSinkOptions opts;
  opts.name = "test";
  opts.batchRecords = 1;
  return std::make_unique<AccessLogSink>(
      std::move(opts),
      encoderFor(tmpl),
      std::make_unique<CallbackTransport>([captured](const folly::IOBuf& data) {
        auto lines = splitLines(data);
        std::lock_guard g(captured->mutex);
        captured->lines.insert(captured->lines.end(), lines.begin(), lines.end());
      })
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

std::unique_ptr<MoqxAccessLog> makeLog(
    std::vector<std::unique_ptr<AccessLogSink>> sinks,
    size_t queueLimit = 1024,
    std::optional<AccessLogServices> services = std::nullopt
) {
  auto resolved = services ? std::move(*services) : allAccessLogSinks(sinks.size());
  return std::make_unique<MoqxAccessLog>(
      AccessLogRelayInfo{},
      std::move(sinks),
      std::move(resolved),
      queueLimit
  );
}

TEST(MoqxAccessLogTest, WritesRecordsInOrderWithSeq) {
  auto captured = std::make_shared<Captured>();
  std::vector<std::unique_ptr<AccessLogSink>> sinks;
  sinks.push_back(callbackSink(captured));
  auto log = makeLog(std::move(sinks));
  log->start();
  auto gate = log->gateSession("live", "viewer", "s1");
  for (auto event :
       {AccessLogEvent::SETUP, AccessLogEvent::SUBSCRIBE, AccessLogEvent::SESSION_END}) {
    log->submit(record(event), log->eventMask(gate, event, false));
  }
  log->stop(std::chrono::steady_clock::now() + 5s);
  EXPECT_EQ(
      captured->get(),
      (std::vector<std::string>{"1 SETUP s1", "2 SUBSCRIBE s1", "3 SESSION_END s1"})
  );
}

TEST(MoqxAccessLogTest, EachServiceSelectsItsSinksEventsAndPeerClasses) {
  std::vector<std::unique_ptr<AccessLogSink>> sinks;
  sinks.push_back(callbackSink(std::make_shared<Captured>()));
  sinks.push_back(callbackSink(std::make_shared<Captured>()));
  AccessLogServices services;
  services.defaults.sinks.push_back(allEvents(0));
  AccessLogSinkSelection viewerRequests;
  viewerRequests.sink = 1;
  viewerRequests.events[size_t(AccessLogEvent::SUBSCRIBE)] = true;
  viewerRequests.peerClasses = {"viewer"};
  auto& live = services.byName["live"];
  live.sinks.push_back(allEvents(0));
  live.sinks.push_back(viewerRequests);
  services.byName["quiet"];
  auto log = makeLog(std::move(sinks), 1024, std::move(services));

  auto liveViewer = log->gateSession("live", "viewer", "a");
  EXPECT_EQ(liveViewer.matched, 0b11u);
  EXPECT_EQ(log->eventMask(liveViewer, AccessLogEvent::SETUP, false), 0b01u);
  EXPECT_EQ(log->eventMask(liveViewer, AccessLogEvent::SUBSCRIBE, false), 0b11u);
  // An error still follows the events and peer classes of each selection.
  EXPECT_EQ(log->eventMask(liveViewer, AccessLogEvent::SETUP, true), 0b01u);
  auto liveRelay = log->gateSession("live", "relay", "b");
  EXPECT_EQ(liveRelay.matched, 0b01u);
  EXPECT_EQ(log->eventMask(liveRelay, AccessLogEvent::SUBSCRIBE, true), 0b01u);
  // A service without its own block uses the defaults.
  EXPECT_EQ(log->gateSession("vod", "viewer", "c").matched, 0b01u);
  EXPECT_EQ(log->gateSession("", "viewer", "c").matched, 0b01u);
  EXPECT_EQ(log->gateSession("quiet", "viewer", "d").matched, 0u);
}

TEST(MoqxAccessLogTest, ErrorsBypassSamplingAndErrorsOnlySinksTakeOnlyErrors) {
  std::vector<std::unique_ptr<AccessLogSink>> sinks;
  sinks.push_back(callbackSink(std::make_shared<Captured>()));
  sinks.push_back(callbackSink(std::make_shared<Captured>()));
  AccessLogServices services;
  services.defaults.sinks.push_back(allEvents(0));
  services.defaults.sinks.push_back(allEvents(1));
  services.defaults.sinks.back().errorsOnly = true;
  auto& unsampled = services.byName["unsampled"];
  unsampled.sessionSample = 0.0;
  unsampled.sinks = services.defaults.sinks;
  auto log = makeLog(std::move(sinks), 1024, std::move(services));

  auto gate = log->gateSession("live", "viewer", "s1");
  EXPECT_TRUE(gate.sampled);
  EXPECT_EQ(log->eventMask(gate, AccessLogEvent::SUBSCRIBE, false), 0b01u);
  EXPECT_EQ(log->eventMask(gate, AccessLogEvent::SUBSCRIBE, true), 0b11u);
  gate = log->gateSession("unsampled", "viewer", "s1");
  EXPECT_EQ(gate.matched, 0b11u);
  EXPECT_FALSE(gate.sampled);
  EXPECT_EQ(log->eventMask(gate, AccessLogEvent::SUBSCRIBE, false), 0u);
  EXPECT_EQ(log->eventMask(gate, AccessLogEvent::SUBSCRIBE, true), 0b11u);
}

TEST(MoqxAccessLogTest, SampleKeyIsDeterministicAndUniform) {
  EXPECT_EQ(accessLogSampleKey("abc"), accessLogSampleKey("abc"));
  size_t below = 0;
  for (int i = 0; i < 1000; ++i) {
    auto key = accessLogSampleKey(std::to_string(i));
    EXPECT_GE(key, 0.0);
    EXPECT_LT(key, 1.0);
    below += key < 0.5;
  }
  EXPECT_GT(below, 400u);
  EXPECT_LT(below, 600u);
}

TEST(MoqxAccessLogTest, OverflowCountsDropsAndReportsThem) {
  auto captured = std::make_shared<Captured>();
  std::vector<std::unique_ptr<AccessLogSink>> sinks;
  sinks.push_back(callbackSink(captured));
  auto log = makeLog(std::move(sinks), /*queueLimit=*/1);
  auto gate = log->gateSession("live", "viewer", "s1");
  auto mask = log->eventMask(gate, AccessLogEvent::SUBSCRIBE, false);
  // The log has not started, so the second and third records overflow the queue.
  log->submit(record(AccessLogEvent::SUBSCRIBE, "a"), mask);
  log->submit(record(AccessLogEvent::SUBSCRIBE, "b"), mask);
  log->submit(record(AccessLogEvent::SUBSCRIBE, "c"), mask);
  EXPECT_EQ(log->queueDropped(), 2u);
  log->start();
  log->stop(std::chrono::steady_clock::now() + 5s);
  EXPECT_EQ(captured->get(), (std::vector<std::string>{"1 LOG_DROPPED 2", "2 SUBSCRIBE a"}));
  log->submit(record(AccessLogEvent::SUBSCRIBE, "d"), mask);
  EXPECT_EQ(log->queueDropped(), 3u);
}

// Each EventBase hands over its records once per loop iteration, so the queue limit is
// checked per batch.
TEST(MoqxAccessLogTest, EventBaseThreadsSubmitInBatches) {
  auto captured = std::make_shared<Captured>();
  std::vector<std::unique_ptr<AccessLogSink>> sinks;
  sinks.push_back(callbackSink(captured));
  auto log = makeLog(std::move(sinks), /*queueLimit=*/1);
  auto gate = log->gateSession("live", "viewer", "s1");
  auto mask = log->eventMask(gate, AccessLogEvent::SUBSCRIBE, false);
  auto submitThree = [&](folly::ScopedEventBaseThread& thread, std::string prefix) {
    folly::Baton<> handedOver;
    auto* evb = thread.getEventBase();
    evb->runInEventBaseThread([&] {
      for (int i = 0; i < 3; ++i) {
        log->submit(record(AccessLogEvent::SUBSCRIBE, prefix + std::to_string(i)), mask);
      }
      // Runs after the access log's loop callback, which was scheduled first.
      evb->runInLoop([&] { handedOver.post(); });
    });
    handedOver.wait();
  };
  folly::ScopedEventBaseThread a("a");
  folly::ScopedEventBaseThread b("b");
  submitThree(a, "a");
  EXPECT_EQ(log->queueDepth(), 3u);
  submitThree(b, "b");
  EXPECT_EQ(log->queueDropped(), 3u);
  log->start();
  log->stop(std::chrono::steady_clock::now() + 5s);
  EXPECT_EQ(
      captured->get(),
      (std::vector<
          std::string>{"1 LOG_DROPPED 3", "2 SUBSCRIBE a0", "3 SUBSCRIBE a1", "4 SUBSCRIBE a2"})
  );
}

TEST(MoqxAccessLogTest, RecordsBufferedOnAnEventBaseSurviveItsDestruction) {
  auto captured = std::make_shared<Captured>();
  std::vector<std::unique_ptr<AccessLogSink>> sinks;
  sinks.push_back(callbackSink(captured));
  auto log = makeLog(std::move(sinks));
  auto gate = log->gateSession("live", "viewer", "s1");
  {
    folly::EventBase evb;
    folly::EventBaseManager::get()->setEventBase(&evb, false);
    SCOPE_EXIT {
      folly::EventBaseManager::get()->clearEventBase();
    };
    log->submit(
        record(AccessLogEvent::SUBSCRIBE, "a"),
        log->eventMask(gate, AccessLogEvent::SUBSCRIBE, false)
    );
    EXPECT_EQ(log->queueDepth(), 0u);
  }
  EXPECT_EQ(log->queueDepth(), 1u);
  log->start();
  log->stop(std::chrono::steady_clock::now() + 5s);
  EXPECT_EQ(captured->get(), (std::vector<std::string>{"1 SUBSCRIBE a"}));
}

TEST(MoqxAccessLogTest, MetricsNameEachSink) {
  auto captured = std::make_shared<Captured>();
  std::vector<std::unique_ptr<AccessLogSink>> sinks;
  sinks.push_back(callbackSink(captured));
  auto log = makeLog(std::move(sinks), /*queueLimit=*/16);
  log->start();
  auto gate = log->gateSession("live", "participant", "s1");
  log->submit(
      record(AccessLogEvent::SUBSCRIBE, "a"),
      log->eventMask(gate, AccessLogEvent::SUBSCRIBE, false)
  );
  auto text =
      folly::coro::blockingWait(stats::accessLogPrometheusText(*log, false))->moveToFbString();
  EXPECT_NE(text.find("# TYPE moqx_access_log_records_total counter"), std::string::npos);
  EXPECT_NE(text.find("moqx_access_log_write_errors_total{sink=\"test\"}"), std::string::npos);
  EXPECT_NE(text.find("moqx_access_log_queue_dropped_total 0"), std::string::npos);
  EXPECT_NE(text.find("moqx_access_log_oldest_record_age_seconds"), std::string::npos);
  log->stop(std::chrono::steady_clock::now() + 5s);

  // The stats stay readable after stop.
  text = folly::coro::blockingWait(stats::accessLogPrometheusText(*log, true))->moveToFbString();
  EXPECT_NE(text.find("moqx_access_log_records_total{sink=\"test\"} 1"), std::string::npos);
}

// Never completes a write unless it is cancelled.
class StuckTransport : public AccessLogTransport {
public:
  explicit StuckTransport(
      std::shared_ptr<std::atomic<bool>> cancelled,
      std::shared_ptr<folly::Baton<>> started = nullptr
  )
      : cancelled_(std::move(cancelled)), started_(std::move(started)) {}

  folly::coro::Task<AccessLogWriteResult> write(const folly::IOBuf&) override {
    if (started_ && !started_->ready()) {
      started_->post();
    }
    auto slept = co_await folly::coro::co_awaitTry(folly::coro::sleep(60s));
    cancelled_->store(slept.hasException());
    co_return AccessLogWriteResult{AccessLogWriteStatus::Retry};
  }
  folly::coro::Task<void> close() override { co_return; }

private:
  std::shared_ptr<std::atomic<bool>> cancelled_;
  std::shared_ptr<folly::Baton<>> started_;
};

TEST(MoqxAccessLogTest, StopCancelsAWriteThatPassesTheDeadline) {
  auto cancelled = std::make_shared<std::atomic<bool>>(false);
  AccessLogSinkOptions opts;
  opts.name = "stuck";
  std::vector<std::unique_ptr<AccessLogSink>> sinks;
  sinks.push_back(std::make_unique<AccessLogSink>(
      std::move(opts),
      encoderFor("$seq $event"),
      std::make_unique<StuckTransport>(cancelled)
  ));
  auto log = makeLog(std::move(sinks));
  log->start();
  auto gate = log->gateSession("live", "viewer", "s1");
  log->submit(
      record(AccessLogEvent::SUBSCRIBE),
      log->eventMask(gate, AccessLogEvent::SUBSCRIBE, false)
  );
  auto start = std::chrono::steady_clock::now();
  log->stop(start + 200ms);
  // Destroying the log waits for the writer to finish.
  log.reset();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 10s);
  EXPECT_TRUE(cancelled->load());
}

TEST(MoqxAccessLogTest, StopCancelsAFlushThatStartedBeforeIt) {
  auto cancelled = std::make_shared<std::atomic<bool>>(false);
  auto started = std::make_shared<folly::Baton<>>();
  AccessLogSinkOptions opts;
  opts.name = "stuck";
  opts.batchRecords = 1;
  std::vector<std::unique_ptr<AccessLogSink>> sinks;
  sinks.push_back(std::make_unique<AccessLogSink>(
      std::move(opts),
      encoderFor("$seq $event"),
      std::make_unique<StuckTransport>(cancelled, started)
  ));
  auto log = makeLog(std::move(sinks));
  log->start();
  auto gate = log->gateSession("live", "viewer", "s1");
  log->submit(
      record(AccessLogEvent::SUBSCRIBE),
      log->eventMask(gate, AccessLogEvent::SUBSCRIBE, false)
  );
  ASSERT_TRUE(started->try_wait_for(5s));
  auto start = std::chrono::steady_clock::now();
  log->stop(start + 200ms);
  log.reset();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 5s);
  EXPECT_TRUE(cancelled->load());
}

TEST(MoqxAccessLogTest, FileTransportWaitsOnAFullFifoUntilCancelled) {
  auto dir =
      std::filesystem::temp_directory_path() / ("moqx-access-fifo-" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  SCOPE_EXIT {
    std::filesystem::remove_all(dir);
  };
  auto path = (dir / "access.fifo").string();
  ASSERT_EQ(::mkfifo(path.c_str(), 0600), 0);
  // The reader stays open and does not read.
  int reader = ::open(path.c_str(), O_RDONLY | O_NONBLOCK);
  ASSERT_GE(reader, 0);
  SCOPE_EXIT {
    ::close(reader);
  };
  auto transport = FileTransport::open(path);
  ASSERT_TRUE(transport.hasValue()) << transport.error();

  constexpr size_t kSize = 4 << 20;
  auto data = folly::IOBuf::copyBuffer(std::string(kSize, 'x'));
  auto start = std::chrono::steady_clock::now();
  auto result =
      folly::coro::blockingWait(folly::coro::timeoutNoDiscard((*transport)->write(*data), 200ms));
  EXPECT_LT(std::chrono::steady_clock::now() - start, 5s);
  EXPECT_EQ(result.status, AccessLogWriteStatus::Retry);
  EXPECT_GT(result.bytesWritten, 0u);
  EXPECT_LT(result.bytesWritten, kSize);
}

TEST(MoqxAccessLogTest, StdoutTransportSurvivesASocketWhoseReaderClosed) {
  int fds[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  ::fflush(stdout);
  int savedStdout = ::dup(STDOUT_FILENO);
  ASSERT_GE(savedStdout, 0);
  // Nothing may print while fd 1 is the socket, so the checks come after fd 1 is restored.
  ::dup2(fds[0], STDOUT_FILENO);
  auto transport = StdoutTransport::open();
  auto line = folly::IOBuf::copyBuffer(std::string("SETUP\n"));
  auto delivered = folly::coro::blockingWait(transport->write(*line));
  char buf[16] = {};
  auto received = ::read(fds[1], buf, sizeof(buf));
  ::close(fds[1]);
  // Without MSG_NOSIGNAL, this write raises SIGPIPE and ends the test binary.
  auto broken = folly::coro::blockingWait(transport->write(*line));
  folly::coro::blockingWait(transport->close());
  ::dup2(savedStdout, STDOUT_FILENO);
  ::close(savedStdout);
  ::close(fds[0]);

  EXPECT_EQ(delivered.status, AccessLogWriteStatus::Ok);
  EXPECT_EQ(std::string(buf, received > 0 ? size_t(received) : 0), "SETUP\n");
  EXPECT_EQ(broken.status, AccessLogWriteStatus::Retry);
}

TEST(MoqxAccessLogTest, FileTransportReopensAfterRotation) {
  auto dir =
      std::filesystem::temp_directory_path() / ("moqx-access-log-" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  SCOPE_EXIT {
    std::filesystem::remove_all(dir);
  };
  auto path = (dir / "access.log").string();
  auto transport = FileTransport::open(path);
  ASSERT_TRUE(transport.hasValue()) << transport.error();
  auto result =
      folly::coro::blockingWait((*transport)->write(*folly::IOBuf::copyBuffer("one\ntwo\n")));
  EXPECT_EQ(result.status, AccessLogWriteStatus::Ok);

  auto rotated = path + ".1";
  std::filesystem::rename(path, rotated);
  (*transport)->reopen();
  folly::coro::blockingWait((*transport)->write(*folly::IOBuf::copyBuffer("three\n")));
  folly::coro::blockingWait((*transport)->close());

  auto read = [](const std::string& p) {
    std::ifstream in(p);
    return std::string(std::istreambuf_iterator<char>(in), {});
  };
  EXPECT_EQ(read(rotated), "one\ntwo\n");
  EXPECT_EQ(read(path), "three\n");
}

TEST(MoqxAccessLogTest, FileTransportReportsAnUnopenablePath) {
  auto transport = FileTransport::open("/nonexistent-moqx-dir/access.log");
  ASSERT_TRUE(transport.hasError());
  EXPECT_FALSE(transport.error().empty());
}

class RetryTransport : public AccessLogTransport {
public:
  folly::coro::Task<AccessLogWriteResult> write(const folly::IOBuf&) override {
    co_return AccessLogWriteResult{AccessLogWriteStatus::Retry, 0ms};
  }
  folly::coro::Task<void> close() override { co_return; }
};

std::unique_ptr<AccessLogSink>
sinkWith(std::unique_ptr<AccessLogTransport> transport, size_t batchRecords = 256) {
  AccessLogSinkOptions opts;
  opts.name = "scripted";
  opts.batchRecords = batchRecords;
  TemplateEncoder encoder;
  encoder.setClassTemplate(AccessLogClass::Request, *AccessLogTemplate::parse("$seq $event"));
  return std::make_unique<AccessLogSink>(std::move(opts), std::move(encoder), std::move(transport));
}

TEST(MoqxAccessLogTest, RetryKeepsABoundedBacklog) {
  auto sink = sinkWith(std::make_unique<RetryTransport>(), /*batchRecords=*/2);
  AccessLogRelayInfo relayInfo;
  for (int i = 0; i < 10; ++i) {
    sink->append(*record(AccessLogEvent::SUBSCRIBE), relayInfo, 0);
  }
  folly::coro::blockingWait(sink->flush());
  EXPECT_EQ(sink->stats().writeErrors, 1u);
  // Four batches of two are kept. The two oldest lines are dropped and leave a seq gap.
  EXPECT_EQ(sink->stats().dropped, 2u);
  EXPECT_TRUE(sink->hasPending());
}

TEST(MoqxAccessLogTest, LinesLostToTheBacklogAreReportedAtClose) {
  auto sink = sinkWith(std::make_unique<RetryTransport>(), /*batchRecords=*/1);
  AccessLogRelayInfo relayInfo;
  for (int i = 0; i < 6; ++i) {
    sink->append(*record(AccessLogEvent::SUBSCRIBE), relayInfo, 0);
  }
  folly::coro::blockingWait(sink->flush());
  EXPECT_EQ(sink->stats().dropped, 2u);
  folly::coro::blockingWait(sink->close(relayInfo, 0));
  // The final LOG_DROPPED line could not be written either, so every pending line counts.
  EXPECT_EQ(sink->stats().dropped, 2u + 5u);
}

// Sends the first `limit` bytes of its first write, then fails. Later writes go through.
class PartialTransport : public AccessLogTransport {
public:
  explicit PartialTransport(size_t limit) : limit_(limit) {}
  folly::coro::Task<AccessLogWriteResult> write(const folly::IOBuf& data) override {
    auto text = data.to<std::string>();
    if (failed_) {
      out += text;
      co_return AccessLogWriteResult{};
    }
    failed_ = true;
    out += text.substr(0, limit_);
    co_return AccessLogWriteResult{AccessLogWriteStatus::Retry, 0ms, limit_};
  }
  folly::coro::Task<void> close() override { co_return; }

  std::string out;

private:
  size_t limit_;
  bool failed_{false};
};

TEST(MoqxAccessLogTest, BacklogStaysBoundedWhileWaitingToRetry) {
  auto sink = sinkWith(std::make_unique<RetryTransport>(), /*batchRecords=*/1);
  AccessLogRelayInfo relayInfo;
  sink->append(*record(AccessLogEvent::SUBSCRIBE), relayInfo, 0);
  folly::coro::blockingWait(sink->flush());
  // The sink waits to retry, and new lines still respect the four-line limit.
  for (int i = 0; i < 10; ++i) {
    sink->append(*record(AccessLogEvent::SUBSCRIBE), relayInfo, 0);
  }
  EXPECT_EQ(sink->stats().dropped, 7u);
}

TEST(MoqxAccessLogTest, ATrimmedLogDroppedLineKeepsItsCount) {
  auto transport = std::make_unique<PartialTransport>(/*limit=*/0);
  auto* raw = transport.get();
  AccessLogSinkOptions opts;
  opts.name = "trimmed";
  opts.batchRecords = 1;
  AccessLogSink sink(std::move(opts), encoderFor("$seq $event"), std::move(transport));
  AccessLogRelayInfo relayInfo;
  // Five queue drops put a LOG_DROPPED line first. The failed write starts a retry wait.
  sink.append(*record(AccessLogEvent::SUBSCRIBE), relayInfo, 5);
  folly::coro::blockingWait(sink.flush());
  // The fifth line pushes the LOG_DROPPED line out of the backlog.
  for (int i = 0; i < 3; ++i) {
    sink.append(*record(AccessLogEvent::SUBSCRIBE), relayInfo, 5);
  }
  folly::coro::blockingWait(sink.close(relayInfo, 5));
  EXPECT_EQ(raw->out, "2 SUBSCRIBE\n3 SUBSCRIBE\n4 SUBSCRIBE\n5 SUBSCRIBE\n6 LOG_DROPPED 5\n");
}

class ReopenCountingTransport : public AccessLogTransport {
public:
  explicit ReopenCountingTransport(std::shared_ptr<std::atomic<int>> reopens)
      : reopens_(std::move(reopens)) {}
  folly::coro::Task<AccessLogWriteResult> write(const folly::IOBuf&) override {
    co_return AccessLogWriteResult{};
  }
  void reopen() override { reopens_->fetch_add(1); }
  folly::coro::Task<void> close() override { co_return; }

private:
  std::shared_ptr<std::atomic<int>> reopens_;
};

TEST(MoqxAccessLogTest, ReopenRunsOnTheWriter) {
  auto reopens = std::make_shared<std::atomic<int>>(0);
  std::vector<std::unique_ptr<AccessLogSink>> sinks;
  sinks.push_back(std::make_unique<AccessLogSink>(
      AccessLogSinkOptions{},
      encoderFor("$event"),
      std::make_unique<ReopenCountingTransport>(reopens)
  ));
  auto log = makeLog(std::move(sinks));
  log->start();
  log->reopen();
  log->stop(std::chrono::steady_clock::now() + 5s);
  EXPECT_EQ(reopens->load(), 1);
}

TEST(MoqxAccessLogTest, APartialWriteResumesWhereItStopped) {
  auto transport = std::make_unique<PartialTransport>(/*limit=*/16);
  auto* raw = transport.get();
  auto sink = sinkWith(std::move(transport));
  AccessLogRelayInfo relayInfo;
  for (int i = 0; i < 3; ++i) {
    sink->append(*record(AccessLogEvent::SUBSCRIBE), relayInfo, 0);
  }
  folly::coro::blockingWait(sink->flush());
  EXPECT_EQ(sink->stats().records, 1u);
  folly::coro::blockingWait(sink->close(relayInfo, 0));
  EXPECT_EQ(raw->out, "1 SUBSCRIBE\n2 SUBSCRIBE\n3 SUBSCRIBE\n");
  EXPECT_EQ(sink->stats().records, 3u);
  EXPECT_EQ(sink->stats().dropped, 0u);
}

TEST(MoqxAccessLogTest, ABacklogDropKeepsAPartlyWrittenLine) {
  auto transport = std::make_unique<PartialTransport>(/*limit=*/3);
  auto* raw = transport.get();
  auto sink = sinkWith(std::move(transport), /*batchRecords=*/1);
  AccessLogRelayInfo relayInfo;
  for (int i = 0; i < 6; ++i) {
    sink->append(*record(AccessLogEvent::SUBSCRIBE), relayInfo, 0);
  }
  folly::coro::blockingWait(sink->flush());
  // Four lines are kept: the rest of line 1, then lines 4 to 6.
  EXPECT_EQ(sink->stats().dropped, 2u);
  folly::coro::blockingWait(sink->close(relayInfo, 0));
  EXPECT_EQ(raw->out, "1 SUBSCRIBE\n4 SUBSCRIBE\n5 SUBSCRIBE\n6 SUBSCRIBE\nLOG_DROPPED\n");
  EXPECT_EQ(sink->stats().records, 5u);
}

TEST(AccessLogSetupTest, SinkSelectionExpandsClassesAndDefaultsToEveryClass) {
  config::AccessLogSinkSelectionConfig selection;
  auto all = makeAccessLogSinkSelection(selection, 0);
  EXPECT_TRUE(all.events[size_t(AccessLogEvent::SETUP)]);
  EXPECT_TRUE(all.events[size_t(AccessLogEvent::FETCH)]);
  EXPECT_FALSE(all.events[size_t(AccessLogEvent::LOG_DROPPED)]);

  EXPECT_FALSE(all.events[size_t(AccessLogEvent::NAMESPACE)]);

  selection.events = {"session", "request", "FETCH", "PUBLISH_BLOCKED"};
  auto some = makeAccessLogSinkSelection(selection, 0);
  EXPECT_TRUE(some.events[size_t(AccessLogEvent::SESSION_END)]);
  EXPECT_TRUE(some.events[size_t(AccessLogEvent::FETCH)]);
  EXPECT_FALSE(some.events[size_t(AccessLogEvent::SUB_STATS)]);
  EXPECT_FALSE(some.events[size_t(AccessLogEvent::NAMESPACE)]);
  EXPECT_TRUE(some.events[size_t(AccessLogEvent::PUBLISH_BLOCKED)]);

  selection.events = {"messages"};
  auto messages = makeAccessLogSinkSelection(selection, 0);
  EXPECT_TRUE(messages.events[size_t(AccessLogEvent::NAMESPACE)]);
  EXPECT_TRUE(messages.events[size_t(AccessLogEvent::NAMESPACE_DONE)]);
  EXPECT_FALSE(messages.events[size_t(AccessLogEvent::SUBSCRIBE)]);
}

TEST(AccessLogSetupTest, ServicesNumberSinksInConfigOrder) {
  config::Config cfg;
  cfg.logging = config::LoggingConfig{};
  cfg.logging->access = config::AccessLogConfig{};
  auto& access = *cfg.logging->access;
  access.sinks.resize(2);
  access.sinks[0].name = "control";
  access.sinks[1].name = "data";
  access.serviceDefaults.sinks = {{.name = "control"}};
  auto& acme = cfg.services["acme"].accessLog;
  acme.sessionSample = 0.5;
  acme.sinks = {{.name = "data", .errorsOnly = true}, {.name = "control"}};

  auto services = makeAccessLogServices(cfg);
  ASSERT_EQ(services.defaults.sinks.size(), 1u);
  EXPECT_EQ(services.defaults.sinks[0].sink, 0u);
  const auto& selected = services.byName.at("acme");
  EXPECT_EQ(selected.sessionSample, 0.5);
  ASSERT_EQ(selected.sinks.size(), 2u);
  EXPECT_EQ(selected.sinks[0].sink, 1u);
  EXPECT_TRUE(selected.sinks[0].errorsOnly);
  EXPECT_EQ(selected.sinks[1].sink, 0u);
}

TEST(AccessLogSetupTest, WithoutSinksTheAccessLogIsDisabled) {
  config::Config cfg;
  auto log = makeAccessLog(cfg);
  ASSERT_TRUE(log.hasValue());
  auto& accessLog = *log.value();
  EXPECT_FALSE(accessLog.enabled());
  accessLog.start();
  accessLog.reopen();
  accessLog.stop(std::chrono::steady_clock::now());
}

TEST(AccessLogSetupTest, UnopenableFileFails) {
  config::Config cfg;
  cfg.logging = config::LoggingConfig{};
  config::AccessLogSinkConfig sink;
  sink.name = "main";
  sink.output = config::AccessLogFileOutput{"/nonexistent-moqx-dir/access.log"};
  cfg.logging->access = config::AccessLogConfig{.queueLimit = 16, .sinks = {sink}};
  auto log = makeAccessLog(cfg);
  ASSERT_TRUE(log.hasError());
  EXPECT_NE(log.error().find("cannot open"), std::string::npos);
}

} // namespace
} // namespace openmoq::moqx::logging
