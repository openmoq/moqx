/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "logging/access/AccessLogRequest.h"
#include "logging/access/AccessLogFilters.h"
#include "logging/access/AccessLogHandles.h"
#include "logging/access/AccessLogPeriodic.h"

#include <algorithm>
#include <folly/coro/BlockingWait.h>
#include <folly/io/async/EventBase.h>

#include <gtest/gtest.h>
#include <moxygen/events/MoQFollyExecutorImpl.h>
#include <moxygen/test/MockMoQSession.h>
#include <moxygen/test/Mocks.h>

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
  std::chrono::milliseconds periodicInterval(const AccessLogSessionGate&) const override {
    return interval;
  }

  std::vector<std::unique_ptr<AccessLogRecord>> records;
  std::chrono::milliseconds interval{0};
  AccessLogSessionGate gate{.matched = 1, .sampled = true};
  bool errorsOnly{false};
};

// Accepts every call and discards it.
class NullSubgroupConsumer : public moxygen::SubgroupConsumer {
public:
  folly::Expected<folly::Unit, moxygen::MoQPublishError>
  object(uint64_t, moxygen::Payload, moxygen::Extensions, bool) override {
    return folly::unit;
  }
  folly::Expected<folly::Unit, moxygen::MoQPublishError>
  beginObject(uint64_t, uint64_t, moxygen::Payload, moxygen::Extensions) override {
    return folly::unit;
  }
  folly::Expected<moxygen::ObjectPublishStatus, moxygen::MoQPublishError>
  objectPayload(moxygen::Payload, bool) override {
    return moxygen::ObjectPublishStatus::IN_PROGRESS;
  }
  folly::Expected<folly::Unit, moxygen::MoQPublishError> endOfGroup(uint64_t) override {
    return folly::unit;
  }
  folly::Expected<folly::Unit, moxygen::MoQPublishError> endOfTrackAndGroup(uint64_t) override {
    return folly::unit;
  }
  folly::Expected<folly::Unit, moxygen::MoQPublishError> endOfSubgroup() override {
    return folly::unit;
  }
  void reset(moxygen::ResetStreamErrorCode) override {}
  folly::Expected<folly::SemiFuture<uint64_t>, moxygen::MoQPublishError>
  awaitReadyToConsume() override {
    return folly::makeSemiFuture<uint64_t>(0);
  }
};

class NullTrackConsumer : public moxygen::TrackConsumer {
public:
  folly::Expected<folly::Unit, moxygen::MoQPublishError> setTrackAlias(moxygen::TrackAlias
  ) override {
    return folly::unit;
  }
  folly::Expected<std::shared_ptr<moxygen::SubgroupConsumer>, moxygen::MoQPublishError>
  beginSubgroup(uint64_t, uint64_t, moxygen::Priority, BeginSubgroupOptions) override {
    return std::make_shared<NullSubgroupConsumer>();
  }
  folly::Expected<folly::SemiFuture<folly::Unit>, moxygen::MoQPublishError>
  awaitStreamCredit() override {
    return folly::makeSemiFuture();
  }
  folly::Expected<folly::Unit, moxygen::MoQPublishError>
  objectStream(const moxygen::ObjectHeader&, moxygen::Payload, bool) override {
    return folly::unit;
  }
  folly::Expected<folly::Unit, moxygen::MoQPublishError>
  datagram(const moxygen::ObjectHeader&, moxygen::Payload, bool) override {
    return folly::unit;
  }
  folly::Expected<folly::Unit, moxygen::MoQPublishError> publishDone(moxygen::PublishDone
  ) override {
    return folly::unit;
  }
};

// A consumer that the session provides, with the counts that the session keeps.
class StatsTrackConsumer : public NullTrackConsumer, public moxygen::MoQTrackStatsSource {
public:
  std::shared_ptr<const moxygen::MoQTrackStats> getTrackStats() const override {
    return trackStats;
  }

  std::shared_ptr<moxygen::MoQTrackStats> trackStats{std::make_shared<moxygen::MoQTrackStats>()};
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
// A session's publisher: its SUBSCRIBE handles have stats.
class FakePublisher : public moxygen::Publisher {
public:
  folly::coro::Task<SubscribeResult>
  subscribe(moxygen::SubscribeRequest, std::shared_ptr<moxygen::TrackConsumer>) override {
    class Handle : public FakeSubscriptionHandle, public moxygen::MoQTrackStatsSource {
    public:
      explicit Handle(std::shared_ptr<moxygen::MoQTrackStats> stats) : stats_(std::move(stats)) {
        setSubscribeOk(moxygen::SubscribeOk{11, 0});
      }
      std::shared_ptr<const moxygen::MoQTrackStats> getTrackStats() const override {
        return stats_;
      }

    private:
      std::shared_ptr<moxygen::MoQTrackStats> stats_;
    };
    co_return std::make_shared<Handle>(trackStats);
  }

  std::shared_ptr<moxygen::MoQTrackStats> trackStats{std::make_shared<moxygen::MoQTrackStats>()};
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

  void fire(AccessLogPeriodic& periodic) {
    periodic.timeoutExpired();
    evb_.loopOnce();
  }

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

TEST_F(AccessLogRequestTest, TxSubscribeThroughTheFilterWritesReceivingSubStats) {
  log_->interval = std::chrono::milliseconds(1000);
  auto sub = moxygen::SubscribeRequest::make(
      moxygen::FullTrackName{moxygen::TrackNamespace({"live"}), "video"}
  );
  auto inner = std::make_shared<FakePublisher>();
  auto publisher = logPublisher(inner, context_, AccessLogDir::Tx);
  auto result = folly::coro::blockingWait(publisher->subscribe(sub, nullptr));
  ASSERT_TRUE(result.hasValue());
  // The filter wraps the reply before ok(), so the request has stats when SUB_STATS starts.
  auto& periodic = AccessLogPeriodic::on(evb_, log_->interval);
  EXPECT_EQ(periodic.size(), 1u);
  inner->trackStats->onGroup(1);
  inner->trackStats->onObject(7);
  fire(periodic);
  ASSERT_EQ(events(), (std::vector{Event::SUBSCRIBE, Event::SUB_STATS}));
  EXPECT_EQ(line(1).dObjects, 1);
  EXPECT_EQ(line(1).dBytes, 7);
  // The relay receives this subscription's data, so the send-only fields are absent.
  EXPECT_FALSE(line(1).dBytesWritten.has_value());
  EXPECT_FALSE(line(1).bytesQueued.has_value());
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

// The session counts an object inside the consumer call that delivers it.
TEST_F(AccessLogRequestTest, AFetchThatEndsOnAnObjectCountsIt) {
  class StatsFetchConsumer : public NiceMock<moxygen::MockFetchConsumer>,
                             public moxygen::MoQTrackStatsSource {
  public:
    std::shared_ptr<const moxygen::MoQTrackStats> getTrackStats() const override {
      return trackStats;
    }
    std::shared_ptr<moxygen::MoQTrackStats> trackStats{std::make_shared<moxygen::MoQTrackStats>()};
  };
  auto session = std::make_shared<StatsFetchConsumer>();
  auto& stats = *session->trackStats;
  ON_CALL(*session, object)
      .WillByDefault([&stats](auto, auto, auto, moxygen::Payload payload, auto, auto, auto) {
        stats.onObject(payload->computeChainDataLength());
        return folly::Expected<folly::Unit, moxygen::MoQPublishError>(folly::unit);
      });
  auto pending = request(Event::FETCH, AccessLogDir::Rx);
  auto consumer = logged(std::shared_ptr<moxygen::FetchConsumer>(session), pending, false);
  pending->ok(std::nullopt);
  consumer->object(0, 0, 0, folly::IOBuf::copyBuffer("payload"), {}, /*finFetch=*/true, false);
  ASSERT_EQ(events(), (std::vector{Event::FETCH}));
  EXPECT_EQ(line(0).objects, 1);
  EXPECT_EQ(line(0).bytes, 7);
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
  update.forward = false;
  auto result = folly::coro::blockingWait(handle->requestUpdate(update));
  EXPECT_TRUE(result.hasValue());
  handle->unsubscribe();
  EXPECT_EQ(inner->unsubscribes, 1);
  ASSERT_EQ(events(), (std::vector{Event::SUBSCRIBE, Event::REQUEST_UPDATE, Event::UNSUBSCRIBE}));
  EXPECT_EQ(line(1).requestID, 6);
  EXPECT_EQ(line(1).status, AccessLogStatus::Ok);
  EXPECT_EQ(line(1).forward, false);
  EXPECT_EQ(line(1).priority, std::nullopt);
}

TEST_F(AccessLogRequestTest, PublishBlockedKeepsAnEmptyTrackName) {
  auto pending = request(Event::SUBSCRIBE_TRACKS, AccessLogDir::Tx, std::nullopt);
  pending->ok(3);
  auto handle = logged(
      std::shared_ptr<moxygen::Publisher::PublishBlockedHandle>(),
      pending,
      /*sessionHeld=*/false
  );
  handle->publishBlocked(moxygen::TrackNamespace({"ch1"}), "");
  ASSERT_EQ(events(), (std::vector{Event::SUBSCRIBE_TRACKS, Event::PUBLISH_BLOCKED}));
  EXPECT_FALSE(line(1).namespaceOnly);
  EXPECT_EQ(line(1).ftn->trackName, "");
}

TEST_F(AccessLogRequestTest, ADroppedNamespaceHandleUnsubscribes) {
  class Inner : public moxygen::Publisher::SubscribeNamespaceHandle {
  public:
    void unsubscribeNamespace() override { ++unsubscribes; }
    folly::coro::Task<RequestUpdateResult> requestUpdate(moxygen::RequestUpdate update) override {
      co_return moxygen::RequestOk{update.requestID};
    }
    int unsubscribes{0};
  };
  auto pending = request(Event::SUBSCRIBE_NAMESPACE, AccessLogDir::Tx, std::nullopt);
  pending->ok(3);
  auto inner = std::make_shared<Inner>();
  auto handle = logged<moxygen::Publisher::SubscribeNamespaceHandle>(inner, pending, false);
  handle.reset();
  EXPECT_EQ(inner->unsubscribes, 1);
  ASSERT_EQ(events(), (std::vector{Event::SUBSCRIBE_NAMESPACE, Event::UNSUBSCRIBE_NAMESPACE}));
  EXPECT_EQ(line(1).endReason, "unsubscribed");
}

TEST_F(AccessLogRequestTest, ANamespaceCancelWithAnErrorCodeIsAnError) {
  using Code = moxygen::RequestErrorCode;
  log_->errorsOnly = true;
  for (auto [code, isError] :
       {std::pair{Code::CANCELLED, false}, std::pair{Code::UNAUTHORIZED, true}}) {
    log_->records.clear();
    auto pending = request(Event::PUBLISH_NAMESPACE, AccessLogDir::Tx, std::nullopt);
    pending->ok(3);
    auto callback = logged(
        std::shared_ptr<moxygen::Subscriber::PublishNamespaceCallback>(),
        pending,
        /*sessionHeld=*/true
    );
    ASSERT_NE(callback, nullptr);
    callback->publishNamespaceCancel(code, "");
    EXPECT_EQ(events().size(), isError ? 1u : 0u);
  }
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

TEST_F(AccessLogRequestTest, EndLineCarriesTheSessionsTotals) {
  auto pending = request(Event::SUBSCRIBE, AccessLogDir::Rx);
  auto session = std::make_shared<StatsTrackConsumer>();
  auto consumer = logged(std::shared_ptr<moxygen::TrackConsumer>(session), pending, false);
  pending->ok(std::nullopt);
  auto& stats = *session->trackStats;
  stats.onGroup(1);
  stats.onObject(10);
  stats.onGroup(2);
  stats.onObject(5);
  stats.subgroups = 1;
  stats.datagrams = 1;
  stats.bytesQueued = 50;
  stats.payloadBytesQueued = 15;
  stats.bytesWritten = 40;
  stats.bytesAcked = 30;
  stats.objectsWritten = 2;
  stats.objectsAcked = 1;
  stats.ackLatencyUsSum = 300;
  stats.ackLatencyUsMax = 300;
  stats.statusObjects = 1;
  stats.onReset(5);
  consumer->publishDone({4, moxygen::PublishDoneStatusCode::TRACK_ENDED, 0, ""});
  ASSERT_EQ(events(), (std::vector{Event::SUBSCRIBE, Event::PUBLISH_DONE}));
  const auto& end = line(1);
  EXPECT_EQ(end.objects, 2);
  EXPECT_EQ(end.bytes, 15);
  EXPECT_EQ(end.groups, 2);
  EXPECT_EQ(end.subgroups, 1);
  EXPECT_EQ(end.datagrams, 1);
  EXPECT_EQ(end.bytesQueued, 50);
  EXPECT_EQ(end.payloadBytesQueued, 15);
  EXPECT_EQ(end.bytesWritten, 40);
  EXPECT_EQ(end.bytesAcked, 30);
  EXPECT_EQ(end.objectsWritten, 2);
  EXPECT_EQ(end.objectsAcked, 1);
  EXPECT_EQ(end.ackUsAvg, 300);
  EXPECT_EQ(end.ackUsMax, 300);
  EXPECT_EQ(end.statusObjects, 1);
  EXPECT_EQ(end.resets, 1);
  EXPECT_EQ(end.lastResetCode, 5);
  EXPECT_TRUE(end.ttfoMs.has_value());
}

TEST_F(AccessLogRequestTest, OnlyTheSessionsOwnObjectsGiveStats) {
  // The relay's consumer on a tx SUBSCRIBE can lead to another session's counts.
  auto tx = request(Event::SUBSCRIBE, AccessLogDir::Tx, std::nullopt);
  auto relays = std::make_shared<StatsTrackConsumer>();
  relays->trackStats->onObject(10);
  auto txConsumer = logged(std::shared_ptr<moxygen::TrackConsumer>(relays), tx, true);
  tx->ok(7);
  tx->ended(Event::UNSUBSCRIBE, Reason::Unsubscribed, false);
  EXPECT_FALSE(line(1).objects.has_value());

  // A tx SUBSCRIBE's handle receives the data, so it does not write bytes to the transport.
  class StatsHandle : public FakeSubscriptionHandle, public moxygen::MoQTrackStatsSource {
  public:
    std::shared_ptr<const moxygen::MoQTrackStats> getTrackStats() const override {
      return trackStats;
    }
    std::shared_ptr<moxygen::MoQTrackStats> trackStats{std::make_shared<moxygen::MoQTrackStats>()};
  };
  auto receiving = request(Event::SUBSCRIBE, AccessLogDir::Tx, std::nullopt);
  auto sessionHandle = std::make_shared<StatsHandle>();
  sessionHandle->trackStats->onObject(10);
  auto handle =
      logged(std::shared_ptr<moxygen::SubscriptionHandle>(sessionHandle), receiving, false);
  receiving->ok(8);
  handle->unsubscribe();
  EXPECT_EQ(line(3).objects, 1);
  EXPECT_FALSE(line(3).bytesWritten.has_value());
}

TEST_F(AccessLogRequestTest, ARequestAddedDuringASweepWaitsForTheNextInterval) {
  log_->interval = std::chrono::milliseconds(1000);
  auto& periodic = AccessLogPeriodic::on(evb_, log_->interval);
  std::vector<std::shared_ptr<AccessLogRequest>> requests;
  std::vector<std::shared_ptr<moxygen::TrackConsumer>> consumers;
  auto start = [&](uint64_t requestID) {
    requests.push_back(request(Event::SUBSCRIBE, AccessLogDir::Rx, requestID));
    consumers.push_back(logged(
        std::shared_ptr<moxygen::TrackConsumer>(std::make_shared<StatsTrackConsumer>()),
        requests.back(),
        false
    ));
    requests.back()->ok(std::nullopt);
  };
  for (uint64_t i = 0; i <= AccessLogPeriodic::kPerLoop; ++i) {
    start(i);
  }
  auto subStats = [&] {
    auto all = events();
    return size_t(std::count(all.begin(), all.end(), Event::SUB_STATS));
  };
  periodic.cancelTimeout();
  periodic.scheduleTimeout(0);
  evb_.loopOnce();
  EXPECT_EQ(subStats(), AccessLogPeriodic::kPerLoop);
  start(1000);
  EXPECT_FALSE(periodic.isScheduled());
  evb_.loopOnce();
  EXPECT_TRUE(periodic.isScheduled());
  EXPECT_EQ(subStats(), AccessLogPeriodic::kPerLoop + 1);
  EXPECT_EQ(periodic.size(), AccessLogPeriodic::kPerLoop + 2);
}

TEST_F(AccessLogRequestTest, PeriodicWritesDeltasUntilTheRequestEnds) {
  log_->interval = std::chrono::milliseconds(1000);
  auto pending = request(Event::SUBSCRIBE, AccessLogDir::Rx);
  auto session = std::make_shared<StatsTrackConsumer>();
  auto consumer = logged(std::shared_ptr<moxygen::TrackConsumer>(session), pending, false);
  pending->ok(std::nullopt);
  auto& periodic = AccessLogPeriodic::on(evb_, log_->interval);
  EXPECT_EQ(periodic.size(), 1u);
  EXPECT_EQ(AccessLogPeriodic::on(evb_, std::chrono::milliseconds(2000)).size(), 0u);
  auto& stats = *session->trackStats;
  stats.onObject(10);
  stats.bytesWritten = 14;
  fire(periodic);
  stats.onObject(6);
  stats.bytesQueued = 30;
  stats.payloadBytesQueued = 16;
  stats.bytesWritten = 24;
  stats.bytesAcked = 14;
  stats.objectsAcked = 2;
  stats.ackLatencyUsSum = 500;
  fire(periodic);
  ASSERT_EQ(events(), (std::vector{Event::SUBSCRIBE, Event::SUB_STATS, Event::SUB_STATS}));
  EXPECT_EQ(line(1).dObjects, 1);
  EXPECT_EQ(line(1).dBytes, 10);
  EXPECT_EQ(line(1).dBytesWritten, 14);
  EXPECT_EQ(line(1).dBytesAcked, 0);
  EXPECT_EQ(line(2).dObjects, 1);
  EXPECT_EQ(line(2).dBytes, 6);
  EXPECT_EQ(line(2).dBytesQueued, 30);
  EXPECT_EQ(line(2).dPayloadBytesQueued, 16);
  EXPECT_EQ(line(2).dBytesWritten, 10);
  EXPECT_EQ(line(2).dBytesAcked, 14);
  EXPECT_EQ(line(2).dObjectsAcked, 2);
  EXPECT_EQ(line(2).dAckUsAvg, 250);
  EXPECT_FALSE(line(1).dAckUsAvg.has_value());
  EXPECT_EQ(line(2).objects, 2);
  EXPECT_EQ(line(2).bytes, 16);
  EXPECT_EQ(line(2).requestID, 4);

  pending->ended(Event::UNSUBSCRIBE, Reason::Unsubscribed, false);
  fire(periodic);
  EXPECT_EQ(periodic.size(), 0u);
  EXPECT_EQ(events().back(), Event::UNSUBSCRIBE);
}

TEST(AccessLogPeriodicTest, IntervalsEndOnWallClockMultiples) {
  using std::chrono::milliseconds;
  using Time = std::chrono::system_clock::time_point;
  auto at = Time(milliseconds(12'345));
  EXPECT_EQ(nextMultiple(at, milliseconds(1000)), Time(milliseconds(13'000)));
  EXPECT_EQ(nextMultiple(at, milliseconds(10'000)), Time(milliseconds(20'000)));
  EXPECT_EQ(
      nextMultiple(Time(milliseconds(13'000)), milliseconds(1000)),
      Time(milliseconds(14'000))
  );
}

TEST_F(AccessLogRequestTest, SessionEndWritesOnce) {
  context_->writeSessionEnd(std::optional<uint64_t>(0));
  context_->writeSessionEnd(*session_);
  ASSERT_EQ(events(), (std::vector{Event::SESSION_END}));
  EXPECT_EQ(line(0).status, AccessLogStatus::Ok);
}

} // namespace
} // namespace openmoq::moqx::logging
