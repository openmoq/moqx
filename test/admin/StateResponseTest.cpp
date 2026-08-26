/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <folly/coro/AsyncPipe.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/WithCancellation.h>
#include <folly/executors/ManualExecutor.h>
#include <folly/io/async/EventBase.h>
#include <proxygen/httpserver/RequestHandler.h>

#include "admin/StateResponse.h"

namespace openmoq::moqx::admin {

namespace {

using ChunkPipe = folly::coro::AsyncPipe<std::unique_ptr<folly::IOBuf>>;

class NoopRequestHandler : public proxygen::RequestHandler {
public:
  void onRequest(std::unique_ptr<proxygen::HTTPMessage>) noexcept override {}
  void onBody(std::unique_ptr<folly::IOBuf>) noexcept override {}
  void onUpgrade(proxygen::UpgradeProtocol) noexcept override {}
  void onEOM() noexcept override {}
  void requestComplete() noexcept override {}
  void onError(proxygen::ProxygenError) noexcept override {}
};

// Records what proxygen would have put on the wire.
class FakeResponseHandler : public proxygen::ResponseHandler {
public:
  FakeResponseHandler() : proxygen::ResponseHandler(&upstream_) {}

  void sendHeaders(proxygen::HTTPMessage& msg) noexcept override {
    status = msg.getStatusCode();
    chunked = msg.getIsChunked();
    contentLength = msg.getHeaders().getSingleOrEmpty(proxygen::HTTP_HEADER_CONTENT_LENGTH);
    ++headerSends;
  }
  void sendChunkHeader(size_t) noexcept override { ++chunkHeaders; }
  void sendBody(std::unique_ptr<folly::IOBuf> b) noexcept override {
    if (b) {
      body += b->moveToFbString().toStdString();
    }
    ++bodySends;
  }
  void sendChunkTerminator() noexcept override {}
  void sendEOM() noexcept override { ++eoms; }
  void sendAbort(folly::Optional<proxygen::ErrorCode>) noexcept override { ++aborts; }
  void refreshTimeout() noexcept override {}
  void pauseIngress() noexcept override {}
  void resumeIngress() noexcept override {}
  folly::Expected<ResponseHandler*, proxygen::ProxygenError>
  newPushedResponse(proxygen::PushHandler*) noexcept override {
    return folly::makeUnexpected(proxygen::kErrorUnknown);
  }
  const wangle::TransportInfo& getSetupTransportInfo() const noexcept override { return info_; }
  void getCurrentTransportInfo(wangle::TransportInfo*) const override {}

  uint16_t status{0};
  bool chunked{false};
  std::string contentLength;
  std::string body;
  int headerSends{0};
  int bodySends{0};
  int chunkHeaders{0};
  int eoms{0};
  int aborts{0};

private:
  NoopRequestHandler upstream_;
  wangle::TransportInfo info_;
};

std::unique_ptr<folly::IOBuf> buf(const std::string& s) {
  return folly::IOBuf::copyBuffer(s);
}

// Loads `chunks` into a closed pipe, optionally failing the stream at the end.
folly::coro::AsyncGenerator<std::unique_ptr<folly::IOBuf>&&>
load(const std::vector<std::string>& chunks, StreamBudget& budget, bool failAtEnd = false) {
  auto [gen, pipe] = ChunkPipe::create();
  for (auto& c : chunks) {
    budget.tryAdd(c.size());
    pipe.write(buf(c));
  }
  if (failAtEnd) {
    std::move(pipe).close(folly::make_exception_wrapper<std::runtime_error>("walk blew up"));
  } else {
    std::move(pipe).close();
  }
  return std::move(gen);
}

class StateResponseTest : public ::testing::Test {
protected:
  // Feeds `chunks` through sendState with egress never paused.
  void
  run(std::vector<std::string> chunks,
      bool failAtEnd = false,
      folly::CancellationToken token = folly::CancellationToken()) {
    auto budget = std::make_shared<StreamBudget>();
    auto gen = load(chunks, *budget, failAtEnd);
    folly::coro::blockingWait(sendState(&ds, std::move(token), std::move(gen), budget, gate));
  }

  // The gate XCHECKs that it is on its own EVB, and folly counts a never-looped
  // EventBase as every thread's, so one satisfies that check from blockingWait
  // or a ManualExecutor alike. Declared first: a coroutine frame left in exec
  // holds a gate reference, and ~TimedBaton reads the EVB.
  folly::EventBase evb;
  folly::ManualExecutor exec;
  FakeResponseHandler ds;
  std::shared_ptr<EgressGate> gate{std::make_shared<EgressGate>(&evb)};
};

} // namespace

TEST_F(StateResponseTest, SingleChunkGoesOutChunked) {
  run({R"({"relay_id":"x"})"});

  EXPECT_EQ(ds.status, 200);
  EXPECT_TRUE(ds.chunked);
  EXPECT_TRUE(ds.contentLength.empty()) << ds.contentLength;
  EXPECT_EQ(ds.body, R"({"relay_id":"x"})");
  EXPECT_EQ(ds.headerSends, 1);
  EXPECT_EQ(ds.chunkHeaders, 1);
  EXPECT_EQ(ds.eoms, 1);
  EXPECT_EQ(ds.aborts, 0);
}

TEST_F(StateResponseTest, MultipleChunksGoOutChunked) {
  run({"{", R"("a":1,)", R"("b":2})"});

  EXPECT_EQ(ds.status, 200);
  EXPECT_TRUE(ds.chunked);
  EXPECT_TRUE(ds.contentLength.empty()) << ds.contentLength;
  EXPECT_EQ(ds.body, R"({"a":1,"b":2})");
  EXPECT_EQ(ds.headerSends, 1) << "headers go out once, on the first flush";
  EXPECT_EQ(ds.chunkHeaders, 3);
  EXPECT_EQ(ds.eoms, 1);
  EXPECT_EQ(ds.aborts, 0);
}

// Deferring the headers is what buys this: nothing was committed yet, so a
// failure can still be reported as a status rather than a truncated body.
TEST_F(StateResponseTest, FailureBeforeHeadersIsAClean500) {
  run({}, /*failAtEnd=*/true);

  EXPECT_EQ(ds.status, 500);
  EXPECT_EQ(ds.body, "internal error\n");
  EXPECT_EQ(ds.aborts, 0);
  EXPECT_EQ(ds.eoms, 1);
}

TEST_F(StateResponseTest, FailureAfterHeadersAborts) {
  // The first chunk commits the headers; the rest never arrives because the
  // stream fails, leaving abort as the only signal.
  run({"{"}, /*failAtEnd=*/true);

  EXPECT_EQ(ds.status, 200);
  EXPECT_EQ(ds.aborts, 1);
  EXPECT_EQ(ds.eoms, 0) << "an aborted response must not also be completed";
}

TEST_F(StateResponseTest, CancelledRequestSendsNothing) {
  folly::CancellationSource source;
  source.requestCancellation();
  run({R"({"relay_id":"x"})"}, /*failAtEnd=*/false, source.getToken());

  EXPECT_EQ(ds.headerSends, 0);
  EXPECT_EQ(ds.bodySends, 0);
  EXPECT_EQ(ds.eoms, 0);
  EXPECT_EQ(ds.aborts, 0);
}

// An empty stream still has to complete the response rather than hang.
TEST_F(StateResponseTest, EmptyStreamStillCompletes) {
  run({});

  EXPECT_EQ(ds.status, 200);
  EXPECT_EQ(ds.eoms, 1);
  EXPECT_EQ(ds.body, "");
}

// The gate is what makes the buffered-bytes cap mean anything: while proxygen
// cannot write, nothing is pulled from the producer at all.
TEST_F(StateResponseTest, PausedEgressStopsPullingUntilResumed) {
  auto budget = std::make_shared<StreamBudget>();
  auto gen = load({"{", R"("a":1,)", R"("b":2})"}, *budget);

  gate->onPaused();
  auto fut = folly::coro::co_withExecutor(
                 &exec,
                 sendState(&ds, folly::CancellationToken(), std::move(gen), budget, gate)
  )
                 .start();
  exec.drain();

  EXPECT_FALSE(fut.isReady()) << "parked on the gate";
  EXPECT_EQ(ds.headerSends, 0);
  EXPECT_EQ(ds.bodySends, 0);

  gate->onResumed();
  exec.drain();

  EXPECT_TRUE(fut.isReady());
  EXPECT_EQ(ds.body, R"({"a":1,"b":2})");
  EXPECT_EQ(ds.eoms, 1);
}

// What onError does: cancel, then shut the gate, then delete the handler. A
// coroutine parked on the gate has to unpark and leave downstream alone.
TEST_F(StateResponseTest, ShutdownWhileParkedSendsNothing) {
  auto budget = std::make_shared<StreamBudget>();
  auto gen = load({R"({"relay_id":"x"})"}, *budget);
  folly::CancellationSource source;

  gate->onPaused();
  auto fut = folly::coro::co_withExecutor(
                 &exec,
                 sendState(&ds, source.getToken(), std::move(gen), budget, gate)
  )
                 .start();
  exec.drain();
  ASSERT_FALSE(fut.isReady());

  source.requestCancellation();
  gate->shutdown();
  exec.drain();

  EXPECT_TRUE(fut.isReady());
  EXPECT_EQ(ds.headerSends, 0);
  EXPECT_EQ(ds.bodySends, 0);
  EXPECT_EQ(ds.eoms, 0);
  EXPECT_EQ(ds.aborts, 0);
}

// A gate that gives up on a request nobody cancelled must still terminate the
// response; abandoning it would hang the client until proxygen timed out.
TEST_F(StateResponseTest, GateGivingUpOnALiveRequestStillTerminates) {
  auto budget = std::make_shared<StreamBudget>();
  auto gen = load({R"({"relay_id":"x"})"}, *budget);

  gate->onPaused();
  auto fut = folly::coro::co_withExecutor(
                 &exec,
                 sendState(&ds, folly::CancellationToken(), std::move(gen), budget, gate)
  )
                 .start();
  exec.drain();
  ASSERT_FALSE(fut.isReady());

  gate->shutdown();
  exec.drain();

  EXPECT_TRUE(fut.isReady());
  EXPECT_EQ(ds.status, 500);
  EXPECT_EQ(ds.eoms, 1);
}

// Resume posts the baton, but folly defers the waiter; a pause landing in that
// window re-arms the baton, so the waiter wakes to a status of notReady. That
// must read as "keep waiting", not as "the request is gone".
TEST_F(StateResponseTest, RepauseBeforeTheWaiterRunsDoesNotAbandonTheResponse) {
  auto budget = std::make_shared<StreamBudget>();
  auto gen = load({R"({"relay_id":"x"})"}, *budget);

  gate->onPaused();
  auto fut = folly::coro::co_withExecutor(
                 &exec,
                 sendState(&ds, folly::CancellationToken(), std::move(gen), budget, gate)
  )
                 .start();
  exec.drain();
  ASSERT_FALSE(fut.isReady());

  gate->onResumed();
  gate->onPaused();
  exec.drain();

  EXPECT_FALSE(fut.isReady()) << "still parked, not abandoned";
  EXPECT_EQ(ds.eoms, 0);
  EXPECT_EQ(ds.aborts, 0);
  EXPECT_EQ(ds.status, 0) << "no 500 either";

  gate->onResumed();
  exec.drain();

  EXPECT_TRUE(fut.isReady());
  EXPECT_EQ(ds.body, R"({"relay_id":"x"})");
  EXPECT_EQ(ds.eoms, 1);
}

TEST_F(StateResponseTest, CancellationWhileParkedSendsNothing) {
  auto budget = std::make_shared<StreamBudget>();
  auto gen = load({R"({"relay_id":"x"})"}, *budget);
  folly::CancellationSource source;

  gate->onPaused();
  auto fut = folly::coro::co_withExecutor(
                 &exec,
                 folly::coro::co_withCancellation(
                     source.getToken(),
                     sendState(&ds, source.getToken(), std::move(gen), budget, gate)
                 )
  )
                 .start();
  exec.drain();
  ASSERT_FALSE(fut.isReady());

  source.requestCancellation();
  exec.drain();

  EXPECT_TRUE(fut.isReady());
  EXPECT_EQ(ds.headerSends, 0);
  EXPECT_EQ(ds.eoms, 0);
}

TEST_F(StateResponseTest, BudgetRejectsPastTheCap) {
  StreamBudget budget(/*cap=*/10);
  EXPECT_TRUE(budget.tryAdd(6));
  EXPECT_TRUE(budget.tryAdd(4));
  EXPECT_FALSE(budget.tryAdd(1)) << "11 > 10";
  budget.release(10);
  EXPECT_TRUE(budget.tryAdd(9)) << "releasing what was sent makes room again";
}

} // namespace openmoq::moqx::admin
