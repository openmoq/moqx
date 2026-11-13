/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "logging/access/AccessLogRequest.h"
#include "logging/access/AccessLogFilters.h"
#include "logging/access/AccessLogHandles.h"

#include <folly/coro/BlockingWait.h>
#include <folly/io/async/EventBase.h>
#include <gtest/gtest.h>
#include <moxygen/events/MoQFollyExecutorImpl.h>
#include <moxygen/test/MockMoQSession.h>

namespace openmoq::moqx::logging {
namespace {

using moxygen::test::MockMoQSession;
using testing::NiceMock;

// Takes every record, so a test sees each line that the session code wrote.
class CapturingLog : public AccessLog {
public:
  AccessLogSessionGate
  gateSession(std::string_view, std::string_view, std::string_view) const override {
    return gate;
  }
  AccessLogSinkMask
  eventMask(const AccessLogSessionGate&, AccessLogEvent, bool isError) const override {
    return !errorsOnly || isError;
  }
  void submit(std::unique_ptr<AccessLogRecord> record, AccessLogSinkMask) override {
    records.push_back(std::move(record));
  }

  std::vector<std::unique_ptr<AccessLogRecord>> records;
  AccessLogSessionGate gate{.matched = 1, .sampled = true};
  bool errorsOnly{false};
};

class FakeSubscriptionHandle : public moxygen::SubscriptionHandle {
public:
  void unsubscribe() override { ++unsubscribes; }
  folly::coro::Task<RequestUpdateResult> requestUpdate(moxygen::RequestUpdate update) override {
    co_return moxygen::RequestOk{update.requestID};
  }
  int unsubscribes{0};
};

// Replies to SUBSCRIBE with a handle whose SUBSCRIBE_OK carries request ID 11.
class FakePublisher : public moxygen::Publisher {
public:
  folly::coro::Task<SubscribeResult>
  subscribe(moxygen::SubscribeRequest, std::shared_ptr<moxygen::TrackConsumer>) override {
    class Handle : public FakeSubscriptionHandle {
    public:
      Handle() { setSubscribeOk(moxygen::SubscribeOk{11, 0}); }
    };
    co_return std::make_shared<Handle>();
  }
};

class AccessLogRequestTest : public testing::Test {
protected:
  void SetUp() override {
    exec_ = std::make_shared<moxygen::MoQFollyExecutorImpl>(&evb_);
    session_ = std::make_shared<NiceMock<MockMoQSession>>(exec_);
    context_ = std::make_shared<AccessLogSessionContext>(log_, AccessLogOpenedBy::Peer);
    context_->setup(session_, "live", "participant");
  }

  std::shared_ptr<AccessLogRequest>
  request(AccessLogEvent startEvent, AccessLogDir dir, std::optional<uint64_t> requestID = 4) {
    AccessLogRecord record;
    record.ftn = std::make_shared<const moxygen::FullTrackName>(
        moxygen::FullTrackName{moxygen::TrackNamespace({"live"}), "video"}
    );
    record.requestID = requestID;
    return std::make_shared<AccessLogRequest>(context_, startEvent, dir, std::move(record));
  }

  std::vector<AccessLogEvent> events() const {
    std::vector<AccessLogEvent> out;
    for (const auto& record : log_->records) {
      out.push_back(record->event);
    }
    return out;
  }

  const AccessLogRecord& line(size_t i) const { return *log_->records.at(i); }

  folly::EventBase evb_;
  std::shared_ptr<moxygen::MoQFollyExecutorImpl> exec_;
  std::shared_ptr<CapturingLog> log_{std::make_shared<CapturingLog>()};
  std::shared_ptr<NiceMock<MockMoQSession>> session_;
  std::shared_ptr<AccessLogSessionContext> context_;
};

moxygen::RequestError requestError() {
  return {7, moxygen::RequestErrorCode::DOES_NOT_EXIST, "no such track"};
}

using Event = AccessLogEvent;
using Reason = AccessLogEndReason;

TEST_F(AccessLogRequestTest, SubscribeWritesStartThenEnd) {
  auto pending = request(AccessLogEvent::SUBSCRIBE, AccessLogDir::Rx);
  pending->ok(std::nullopt);
  pending->ended(Event::UNSUBSCRIBE, Reason::Unsubscribed, false);
  pending->ended(Event::PUBLISH_DONE, Reason::PublishDone, false, 2);
  pending.reset();
  ASSERT_EQ(events(), (std::vector{Event::SUBSCRIBE, Event::UNSUBSCRIBE}));
  EXPECT_EQ(line(0).status, AccessLogStatus::Ok);
  EXPECT_EQ(line(0).requestID, 4);
  EXPECT_EQ(line(0).dir, AccessLogDir::Rx);
  EXPECT_EQ(line(1).endReason, "unsubscribed");
}

TEST_F(AccessLogRequestTest, EndBeforeReplyWaitsForStartLine) {
  auto pending = request(AccessLogEvent::SUBSCRIBE, AccessLogDir::Rx);
  pending->ended(Event::PUBLISH_DONE, Reason::PublishDone, false, 2);
  EXPECT_TRUE(log_->records.empty());
  pending->ok(std::nullopt);
  EXPECT_EQ(events(), (std::vector{Event::SUBSCRIBE, Event::PUBLISH_DONE}));
}

TEST_F(AccessLogRequestTest, ErrorWritesOneLine) {
  auto pending = request(AccessLogEvent::SUBSCRIBE, AccessLogDir::Rx);
  pending->error(std::nullopt, requestError());
  pending->ended(Event::UNSUBSCRIBE, Reason::Unsubscribed, false);
  pending.reset();
  ASSERT_EQ(events(), (std::vector{Event::SUBSCRIBE}));
  EXPECT_EQ(line(0).status, AccessLogStatus::Error);
  EXPECT_EQ(line(0).reason, "no such track");
}

TEST_F(AccessLogRequestTest, ReleasedWithoutReplyIsAborted) {
  request(AccessLogEvent::PUBLISH_NAMESPACE, AccessLogDir::Rx).reset();
  ASSERT_EQ(events(), (std::vector{Event::PUBLISH_NAMESPACE}));
  EXPECT_EQ(line(0).status, AccessLogStatus::Aborted);
}

TEST_F(AccessLogRequestTest, TxRequestTakesIdFromReply) {
  auto pending = request(AccessLogEvent::SUBSCRIBE, AccessLogDir::Tx, std::nullopt);
  pending->ok(9);
  ASSERT_EQ(events(), (std::vector{Event::SUBSCRIBE}));
  EXPECT_EQ(line(0).requestID, 9);
  EXPECT_EQ(line(0).dir, AccessLogDir::Tx);
}

TEST_F(AccessLogRequestTest, PublisherFilterTakesRxIdFromRequestAndTxIdFromReply) {
  auto sub = moxygen::SubscribeRequest::make(
      moxygen::FullTrackName{moxygen::TrackNamespace({"live"}), "video"}
  );
  sub.requestID = 4;
  // Each handle stays alive, so neither request writes an end line.
  std::vector<std::shared_ptr<moxygen::SubscriptionHandle>> handles;
  for (auto dir : {AccessLogDir::Rx, AccessLogDir::Tx}) {
    auto publisher = logPublisher(std::make_shared<FakePublisher>(), context_, dir);
    auto result = folly::coro::blockingWait(publisher->subscribe(sub, nullptr));
    ASSERT_TRUE(result.hasValue());
    handles.push_back(std::move(result.value()));
  }
  ASSERT_EQ(events(), (std::vector{Event::SUBSCRIBE, Event::SUBSCRIBE}));
  EXPECT_EQ(line(0).requestID, 4);
  EXPECT_EQ(line(1).requestID, 11);
  EXPECT_EQ(line(1).dir, AccessLogDir::Tx);
}

TEST_F(AccessLogRequestTest, FilterIsSkippedWithoutContext) {
  auto inner = std::make_shared<FakePublisher>();
  EXPECT_EQ(logPublisher(inner, nullptr, AccessLogDir::Rx), inner);
}

TEST_F(AccessLogRequestTest, FilterIsSkippedWithoutContextOrWhenFilteredOut) {
  auto inner = std::make_shared<FakePublisher>();
  EXPECT_EQ(logPublisher(inner, nullptr, AccessLogDir::Rx), inner);
  EXPECT_NE(logPublisher(inner, context_, AccessLogDir::Rx), inner);

  log_->gate = {0, 0};
  auto filteredOut = std::make_shared<AccessLogSessionContext>(log_, AccessLogOpenedBy::Peer);
  EXPECT_NE(logPublisher(inner, filteredOut, AccessLogDir::Rx), inner);
  filteredOut->setup(session_, "live", "participant");
  EXPECT_EQ(logPublisher(inner, filteredOut, AccessLogDir::Rx), inner);
}

// The session ended an open request itself, as with PUBLISH_DONE TOO_FAR_BEHIND.
TEST_F(AccessLogRequestTest, ReleasedWhileOpenEndsOnRelaySide) {
  auto pending = request(AccessLogEvent::SUBSCRIBE, AccessLogDir::Rx);
  pending->ok(std::nullopt);
  pending->releasedBySession();
  ASSERT_EQ(events(), (std::vector{Event::SUBSCRIBE, Event::PUBLISH_DONE}));
  EXPECT_EQ(line(1).endReason, "?");
}

TEST_F(AccessLogRequestTest, SessionCloseEndsOnPeerSide) {
  auto rx = request(AccessLogEvent::SUBSCRIBE, AccessLogDir::Rx);
  auto tx = request(AccessLogEvent::SUBSCRIBE, AccessLogDir::Tx, std::nullopt);
  rx->ok(std::nullopt);
  tx->ok(3);
  session_->close(moxygen::SessionCloseErrorCode::NO_ERROR);
  // Teardown reaches the relay's handle and consumer as an unsubscribe and a PUBLISH_DONE.
  rx->ended(Event::UNSUBSCRIBE, Reason::Unsubscribed, false);
  tx->ended(Event::PUBLISH_DONE, Reason::PublishDone, true, 0);
  ASSERT_EQ(
      events(),
      (std::vector{Event::SUBSCRIBE, Event::SUBSCRIBE, Event::UNSUBSCRIBE, Event::PUBLISH_DONE})
  );
  EXPECT_EQ(line(2).endReason, "session_closed");
  EXPECT_EQ(line(3).endReason, "session_closed");
}

TEST_F(AccessLogRequestTest, ShutdownEndsOnRelaySide) {
  auto pending = request(AccessLogEvent::SUBSCRIBE, AccessLogDir::Rx);
  pending->ok(std::nullopt);
  log_->beginShutdown();
  pending.reset();
  ASSERT_EQ(events(), (std::vector{Event::SUBSCRIBE, Event::PUBLISH_DONE}));
  EXPECT_EQ(line(1).endReason, "shutdown");
}

TEST_F(AccessLogRequestTest, FetchWritesOneLineAfterReplyAndData) {
  auto pending = request(AccessLogEvent::FETCH, AccessLogDir::Rx);
  pending->ended(Event::FETCH, Reason::Fin, false);
  EXPECT_TRUE(log_->records.empty());
  pending->ok(std::nullopt);
  pending.reset();
  ASSERT_EQ(events(), (std::vector{Event::FETCH}));
  EXPECT_EQ(line(0).status, AccessLogStatus::Ok);
  EXPECT_EQ(line(0).endReason, "fin");
}

// A draft-18 FETCH's data stream can finish, and the session release its consumer, before
// FETCH_OK arrives.
TEST_F(AccessLogRequestTest, FetchReleasedBeforeReplyWaitsForIt) {
  auto pending = request(Event::FETCH, AccessLogDir::Tx, std::nullopt);
  pending->ended(Event::FETCH, Reason::Fin, false);
  pending->releasedBySession();
  EXPECT_TRUE(log_->records.empty());
  pending->ok(3);
  ASSERT_EQ(events(), (std::vector{Event::FETCH}));
  EXPECT_EQ(line(0).status, AccessLogStatus::Ok);
  EXPECT_EQ(line(0).requestID, 3);
}

// A PUBLISH's subscriber sends its updates, so the relay's update on a peer's PUBLISH is tx.
TEST_F(AccessLogRequestTest, PublishUpdateFlowsFromTheSubscriber) {
  auto pending = request(Event::PUBLISH, AccessLogDir::Rx);
  pending->ok(std::nullopt);
  auto inner = std::make_shared<FakeSubscriptionHandle>();
  auto handle = logged<moxygen::SubscriptionHandle>(inner, pending, /*sessionHeld=*/false);
  moxygen::RequestUpdate update;
  update.existingRequestID = 4;
  ASSERT_TRUE(folly::coro::blockingWait(handle->requestUpdate(update)).hasValue());
  ASSERT_EQ(events(), (std::vector{Event::PUBLISH, Event::REQUEST_UPDATE}));
  EXPECT_EQ(line(1).dir, AccessLogDir::Tx);
}

// A relay's SUBSCRIBE_TRACKS without a PublishBlockedHandle still ends when the session
// releases the wrapper that it gets in its place.
TEST_F(AccessLogRequestTest, NullBlockedHandleStillEndsTheRequest) {
  auto pending = request(Event::SUBSCRIBE_TRACKS, AccessLogDir::Tx, std::nullopt);
  auto handle = logged(
      std::shared_ptr<moxygen::Publisher::PublishBlockedHandle>(),
      pending,
      /*sessionHeld=*/true
  );
  ASSERT_NE(handle, nullptr);
  pending->ok(3);
  session_->close(moxygen::SessionCloseErrorCode::NO_ERROR);
  handle.reset();
  ASSERT_EQ(events(), (std::vector{Event::SUBSCRIBE_TRACKS, Event::UNSUBSCRIBE_TRACKS}));
  EXPECT_EQ(line(1).endReason, "session_closed");
}

TEST_F(AccessLogRequestTest, FetchErrorAndReset) {
  auto failed = request(AccessLogEvent::FETCH, AccessLogDir::Rx);
  failed->error(std::nullopt, requestError());
  auto reset = request(AccessLogEvent::FETCH, AccessLogDir::Tx, std::nullopt);
  reset->ok(5);
  reset->ended(Event::FETCH, Reason::Reset, true, 3);
  ASSERT_EQ(events(), (std::vector{Event::FETCH, Event::FETCH}));
  EXPECT_EQ(line(0).endReason, "request_error");
  EXPECT_EQ(line(1).status, AccessLogStatus::Error);
  EXPECT_EQ(line(1).errorCode, 3);
}

TEST_F(AccessLogRequestTest, FetchStatusFollowsWhetherItsEndIsAnError) {
  auto cancelled = request(AccessLogEvent::FETCH, AccessLogDir::Rx);
  cancelled->ok(std::nullopt);
  cancelled->ended(Event::FETCH, Reason::Cancelled, false);
  auto pending = request(AccessLogEvent::FETCH, AccessLogDir::Rx);
  log_->errorsOnly = true;
  session_->close(moxygen::SessionCloseErrorCode::NO_ERROR);
  pending.reset();
  ASSERT_EQ(events(), (std::vector{Event::FETCH, Event::FETCH}));
  EXPECT_EQ(line(0).status, AccessLogStatus::Ok);
  EXPECT_EQ(line(0).endReason, "cancelled");
  EXPECT_EQ(line(1).status, AccessLogStatus::Aborted);
}

TEST_F(AccessLogRequestTest, SessionHeldHandleEndsRequestOnRelease) {
  auto pending = request(AccessLogEvent::SUBSCRIBE, AccessLogDir::Rx);
  pending->ok(std::nullopt);
  auto inner = std::make_shared<FakeSubscriptionHandle>();
  auto handle = logged<moxygen::SubscriptionHandle>(inner, pending, /*sessionHeld=*/true);
  pending.reset();
  EXPECT_EQ(events(), (std::vector{Event::SUBSCRIBE}));
  handle.reset();
  ASSERT_EQ(events(), (std::vector{Event::SUBSCRIBE, Event::PUBLISH_DONE}));
}

TEST_F(AccessLogRequestTest, HandleWritesUnsubscribeAndRequestUpdate) {
  auto pending = request(AccessLogEvent::SUBSCRIBE, AccessLogDir::Rx);
  pending->ok(std::nullopt);
  auto inner = std::make_shared<FakeSubscriptionHandle>();
  auto handle = logged<moxygen::SubscriptionHandle>(inner, pending, /*sessionHeld=*/true);
  moxygen::RequestUpdate update;
  update.requestID = 6;
  update.existingRequestID = 4;
  auto result = folly::coro::blockingWait(handle->requestUpdate(update));
  EXPECT_TRUE(result.hasValue());
  handle->unsubscribe();
  EXPECT_EQ(inner->unsubscribes, 1);
  ASSERT_EQ(events(), (std::vector{Event::SUBSCRIBE, Event::REQUEST_UPDATE, Event::UNSUBSCRIBE}));
  EXPECT_EQ(line(1).requestID, 6);
  EXPECT_EQ(line(1).status, AccessLogStatus::Ok);
}

TEST_F(AccessLogRequestTest, NamespaceMessagesExtendThePrefix) {
  auto pending = request(Event::SUBSCRIBE_NAMESPACE, AccessLogDir::Tx, std::nullopt);
  pending->ok(2);
  class NullHandle : public moxygen::Publisher::NamespacePublishHandle {
  public:
    void namespaceMsg(const moxygen::TrackNamespace&) override {}
    void namespaceDoneMsg(const moxygen::TrackNamespace&) override {}
  };
  auto handle = logged<moxygen::Publisher::NamespacePublishHandle>(
      std::make_shared<NullHandle>(),
      pending,
      true
  );
  handle->namespaceMsg(moxygen::TrackNamespace({"ch1"}));
  handle->namespaceDoneMsg(moxygen::TrackNamespace({"ch1"}));
  ASSERT_EQ(
      events(),
      (std::vector{Event::SUBSCRIBE_NAMESPACE, Event::NAMESPACE, Event::NAMESPACE_DONE})
  );
  EXPECT_EQ(
      line(1).ftn->trackNamespace,
      moxygen::TrackNamespace(std::vector<std::string>{"live", "ch1"})
  );
  EXPECT_TRUE(line(1).namespaceOnly);
  EXPECT_EQ(line(1).requestID, 2);
  EXPECT_EQ(line(1).dir, AccessLogDir::Tx);
}

TEST_F(AccessLogRequestTest, SessionEndWritesOnce) {
  context_->writeSessionEnd(std::optional<uint64_t>(0));
  context_->writeSessionEnd(*session_);
  ASSERT_EQ(events(), (std::vector{Event::SESSION_END}));
  EXPECT_EQ(line(0).status, AccessLogStatus::Ok);
}

} // namespace
} // namespace openmoq::moqx::logging
