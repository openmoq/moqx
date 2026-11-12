/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/SessionParticipant.h"

#include "MockClusterPeer.h"

#include "MoqxRelay.h"
#include "peering/MoqtRelayBackend.h"
#include "relay/MoqClusterRouteCodec.h"
#include "relay/PublisherCrossExecFilter.h"

#include <folly/coro/BlockingWait.h>
#include <folly/executors/ManualExecutor.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <moxygen/MoQVersions.h>
#include <moxygen/events/MoQFollyExecutorImpl.h>
#include <moxygen/test/MockMoQSession.h>
#include <moxygen/test/Mocks.h>

using namespace testing;
using namespace moxygen;
using namespace openmoq::moqx;
using moxygen::test::MockMoQSession;

namespace {

// Each session runs on its own EventBase thread, as in the threaded relay modes. The test thread
// stands in for the relay thread.
class SessionParticipantTest : public ::testing::Test {
protected:
  std::shared_ptr<MockMoQSession> makeSession() {
    auto session = std::make_shared<NiceMock<MockMoQSession>>(exec_);
    ON_CALL(*session, getNegotiatedVersion())
        .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft16)));
    openmoq::moqx::test::negotiateClusterPeer(*session, 7, 3);
    return session;
  }

  template <typename Fn> void onSessionThread(Fn&& fn) {
    sessionThread_.getEventBase()->runInEventBaseThreadAndWait(std::forward<Fn>(fn));
  }

  // MoQSession tears down on its own executor.
  void destroyOnSessionThread(std::shared_ptr<MockMoQSession>& session) {
    onSessionThread([&] { session.reset(); });
  }

  static SubscribeRequest makeSubscribe() {
    SubscribeRequest sub;
    sub.fullTrackName = FullTrackName{TrackNamespace{{"ns"}}, "track"};
    sub.requestID = RequestID(1);
    return sub;
  }

  folly::ManualExecutor relayExec_;
  folly::ScopedEventBaseThread sessionThread_{"session"};
  std::shared_ptr<MoQFollyExecutorImpl> exec_ =
      std::make_shared<MoQFollyExecutorImpl>(sessionThread_.getEventBase());
};

TEST_F(SessionParticipantTest, SnapshotMatchesSession) {
  auto session = makeSession();
  onSessionThread([&] {
    auto info = SessionParticipant::snapshot(*session);
    EXPECT_EQ(info.sessionId, session->sessionId());
    EXPECT_EQ(info.version, kVersionDraft16);
    EXPECT_EQ(info.executor, exec_.get());
    EXPECT_TRUE(info.routing.relayHops);
    EXPECT_EQ(info.routing.peerHopID, 7);
    EXPECT_EQ(info.routing.linkCost, 3);
  });
  destroyOnSessionThread(session);
}

TEST_F(SessionParticipantTest, AttachesParticipantToEachRequest) {
  auto session = makeSession();
  auto relayPublisher = std::make_shared<NiceMock<MockPublisher>>();
  auto relaySubscriber = std::make_shared<NiceMock<MockSubscriber>>();
  auto participant =
      std::make_shared<SessionParticipant>(relayPublisher, relaySubscriber, &relayExec_);
  std::vector<SessionId> seen;
  auto record = [&] { seen.push_back(requestParticipant().info->sessionId); };
  auto fail = [](RequestID id) {
    return folly::makeUnexpected(RequestError{id, RequestErrorCode::DOES_NOT_EXIST, "test"});
  };
  EXPECT_CALL(*relayPublisher, trackStatus(_)).WillOnce([&](TrackStatus req) {
    record();
    return folly::coro::makeTask<Publisher::TrackStatusResult>(fail(req.requestID));
  });
  EXPECT_CALL(*relayPublisher, subscribe(_, _)).WillOnce([&](SubscribeRequest sub, auto) {
    record();
    return folly::coro::makeTask<Publisher::SubscribeResult>(fail(sub.requestID));
  });
  EXPECT_CALL(*relayPublisher, fetch(_, _)).WillOnce([&](Fetch fetch, auto) {
    record();
    return folly::coro::makeTask<Publisher::FetchResult>(fail(fetch.requestID));
  });
  EXPECT_CALL(*relayPublisher, subscribeNamespace(_, _))
      .WillOnce([&](SubscribeNamespace subNs, auto) {
        record();
        return folly::coro::makeTask<Publisher::SubscribeNamespaceResult>(fail(subNs.requestID));
      });
  EXPECT_CALL(*relayPublisher, subscribeTracks(_, _))
      .WillOnce([&](SubscribeTracks subTracks, auto) {
        record();
        return folly::coro::makeTask<Publisher::SubscribeTracksResult>(fail(subTracks.requestID));
      });
  EXPECT_CALL(*relaySubscriber, publishNamespace(_, _)).WillOnce([&](PublishNamespace pubNs, auto) {
    record();
    return folly::coro::makeTask<Subscriber::PublishNamespaceResult>(fail(pubNs.requestID));
  });
  EXPECT_CALL(*relaySubscriber, publish(_, _))
      .WillOnce([&](PublishRequest pub, auto) -> Subscriber::PublishResult {
        record();
        return fail(pub.requestID);
      });
  // Each request gets a fresh request context, as MoQSession gives it.
  onSessionThread([&] {
    session->setPublishHandler(participant);
    {
      folly::RequestContextScopeGuard guard;
      (void)participant->trackStatus(TrackStatus{});
    }
    {
      folly::RequestContextScopeGuard guard;
      (void)folly::coro::blockingWait(participant->subscribe(makeSubscribe(), nullptr));
    }
    {
      folly::RequestContextScopeGuard guard;
      (void)participant->fetch(Fetch{}, nullptr);
    }
    {
      folly::RequestContextScopeGuard guard;
      (void)participant->subscribeNamespace(SubscribeNamespace{}, nullptr);
    }
    {
      folly::RequestContextScopeGuard guard;
      (void)participant->subscribeTracks(SubscribeTracks{}, nullptr);
    }
    {
      folly::RequestContextScopeGuard guard;
      (void)participant->publishNamespace(PublishNamespace{}, nullptr);
    }
    {
      folly::RequestContextScopeGuard guard;
      (void)participant->publish(PublishRequest{}, nullptr);
    }
  });
  EXPECT_EQ(seen, std::vector<SessionId>(7, session->sessionId()));
  destroyOnSessionThread(session);
}

// The participant attached to a request carries the routing that the peering backend chose for a
// session that negotiated MoQ Cluster.
TEST_F(SessionParticipantTest, AttachedParticipantCarriesTheBackendsRouting) {
  auto session = makeSession();
  auto relay = std::make_shared<MoqxRelay>(config::CacheConfig{});
  MoqtRelayBackend backend(relay, config::ServiceConfig{}, /*hopID=*/900);
  auto relayPublisher = std::make_shared<NiceMock<MockPublisher>>();
  auto participant = std::make_shared<SessionParticipant>(
      relayPublisher,
      std::make_shared<NiceMock<MockSubscriber>>(),
      &relayExec_,
      &backend
  );
  std::optional<ParticipantRouting> routing;
  EXPECT_CALL(*relayPublisher, trackStatus(_)).WillOnce([&](TrackStatus req) {
    routing = requestParticipant().info->routing;
    return folly::coro::makeTask<Publisher::TrackStatusResult>(
        folly::makeUnexpected(RequestError{req.requestID, RequestErrorCode::DOES_NOT_EXIST, "test"})
    );
  });
  onSessionThread([&] {
    session->setPublishHandler(participant);
    folly::RequestContextScopeGuard guard;
    (void)participant->trackStatus(TrackStatus{});
  });
  ASSERT_TRUE(routing);
  EXPECT_TRUE(routing->fromPeering);
  EXPECT_TRUE(routing->wantsUpdates);
  EXPECT_NE(dynamic_cast<const MoqClusterRouteCodec*>(routing->codec.get()), nullptr);
  ASSERT_TRUE(routing->exclusion);
  EXPECT_EQ(routing->exclusion->hop, 7);
  destroyOnSessionThread(session);
}

TEST_F(SessionParticipantTest, RequestUpdateOnASubscriptionCarriesTheParticipant) {
  auto session = makeSession();
  auto relayPublisher = std::make_shared<NiceMock<MockPublisher>>();
  auto participant = std::make_shared<SessionParticipant>(
      relayPublisher,
      std::make_shared<NiceMock<MockSubscriber>>(),
      &relayExec_
  );
  SubscribeOk ok;
  ok.requestID = RequestID(1);
  auto inner = std::make_shared<NiceMock<MockSubscriptionHandle>>(ok);
  EXPECT_CALL(*relayPublisher, subscribe(_, _)).WillOnce([&](auto, auto) {
    return folly::coro::makeTask<Publisher::SubscribeResult>(inner);
  });
  std::optional<SessionId> seen;
  EXPECT_CALL(*inner, requestUpdateCalled(_)).WillOnce([&](auto) {
    seen = requestParticipant().info->sessionId;
  });
  ON_CALL(*inner, requestUpdateResult()).WillByDefault(Return(RequestOk{}));
  onSessionThread([&] {
    session->setPublishHandler(participant);
    std::shared_ptr<SubscriptionHandle> handle;
    {
      folly::RequestContextScopeGuard guard;
      handle = folly::coro::blockingWait(
                   participant->subscribe(makeSubscribe(), std::make_shared<MockTrackConsumer>())
      )
                   .value();
    }
    folly::RequestContextScopeGuard guard;
    (void)folly::coro::blockingWait(handle->requestUpdate(RequestUpdate{}));
  });
  EXPECT_EQ(seen, session->sessionId());
  destroyOnSessionThread(session);
}

TEST_F(SessionParticipantTest, ParticipantScopeAttachesTheParticipant) {
  auto info = std::make_shared<const ParticipantInfo>(ParticipantInfo{.sessionId = SessionId(42)});
  ParticipantScope scope(Participant(info, nullptr, nullptr));
  EXPECT_EQ(requestParticipant().info, info);
}

TEST_F(SessionParticipantTest, BindingToASecondSessionFails) {
  testing::FLAGS_gtest_death_test_style = "threadsafe";
  auto participant = std::make_shared<SessionParticipant>(
      std::make_shared<NiceMock<MockPublisher>>(),
      std::make_shared<NiceMock<MockSubscriber>>(),
      /*relayExec=*/nullptr
  );
  auto first = makeSession();
  auto second = makeSession();
  onSessionThread([&] { first->setPublishHandler(participant); });
  EXPECT_DEATH(
      onSessionThread([&] { second->setPublishHandler(participant); }),
      "more than one session"
  );
  destroyOnSessionThread(first);
  destroyOnSessionThread(second);
}

TEST_F(SessionParticipantTest, CallAfterSessionEndsReportsClosed) {
  auto session = makeSession();
  Participant participant;
  onSessionThread([&] { participant = SessionParticipant::makeParticipant(session, &relayExec_); });
  destroyOnSessionThread(session);

  auto result = folly::coro::blockingWait(
      participant.publisher->subscribe(makeSubscribe(), std::make_shared<MockTrackConsumer>())
  );
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error().errorCode, SubscribeErrorCode::INTERNAL_ERROR);
  EXPECT_EQ(result.error().reasonPhrase, "session closed");
}

TEST_F(SessionParticipantTest, PublishNamespaceAfterSessionEndsReportsClosed) {
  auto session = makeSession();
  Participant participant;
  onSessionThread([&] { participant = SessionParticipant::makeParticipant(session, &relayExec_); });
  destroyOnSessionThread(session);

  PublishNamespace pubNs;
  pubNs.requestID = RequestID(1);
  auto result =
      folly::coro::blockingWait(participant.subscriber->publishNamespace(std::move(pubNs), nullptr)
      );
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error().errorCode, PublishNamespaceErrorCode::INTERNAL_ERROR);
  EXPECT_EQ(result.error().reasonPhrase, "session closed");
}

TEST_F(SessionParticipantTest, CallFromAnotherThreadRunsOnTheSessionThread) {
  auto session = makeSession();
  Participant participant;
  onSessionThread([&] { participant = SessionParticipant::makeParticipant(session, &relayExec_); });
  bool onSessionThreadInCall = false;
  EXPECT_CALL(*session, subscribe(_, _)).WillOnce([&](SubscribeRequest sub, auto) {
    onSessionThreadInCall = sessionThread_.getEventBase()->isInEventBaseThread();
    return folly::coro::makeTask<Publisher::SubscribeResult>(folly::makeUnexpected(
        SubscribeError{sub.requestID, SubscribeErrorCode::DOES_NOT_EXIST, "test"}
    ));
  });

  (void)folly::coro::blockingWait(
      participant.publisher->subscribe(makeSubscribe(), std::make_shared<MockTrackConsumer>())
  );
  EXPECT_TRUE(onSessionThreadInCall);
  destroyOnSessionThread(session);
}

TEST_F(SessionParticipantTest, RunOnExecutorCallsTheSessionWithoutAHop) {
  auto session = makeSession();
  Participant participant;
  onSessionThread([&] { participant = SessionParticipant::makeParticipant(session, &relayExec_); });
  bool onSessionThreadInCall = false;
  EXPECT_CALL(*session, subscribe(_, _)).WillOnce([&](SubscribeRequest sub, auto) {
    onSessionThreadInCall = sessionThread_.getEventBase()->isInEventBaseThread();
    return folly::coro::makeTask<Publisher::SubscribeResult>(folly::makeUnexpected(
        SubscribeError{sub.requestID, SubscribeErrorCode::DOES_NOT_EXIST, "test"}
    ));
  });

  folly::coro::blockingWait(
      participant.runOnExecutor([&](Participant::OnExecutor onExecutor) -> folly::coro::Task<void> {
        EXPECT_EQ(dynamic_cast<PublisherCrossExecFilter*>(&onExecutor.publisher()), nullptr);
        (void)co_await onExecutor.publisher().subscribe(
            makeSubscribe(),
            std::make_shared<MockTrackConsumer>()
        );
      })
  );
  EXPECT_TRUE(onSessionThreadInCall);
  destroyOnSessionThread(session);
}

TEST_F(SessionParticipantTest, ParticipantDoesNotOwnTheSession) {
  auto session = makeSession();
  const auto owners = session.use_count();
  Participant participant;
  onSessionThread([&] { participant = SessionParticipant::makeParticipant(session, &relayExec_); });
  EXPECT_EQ(session.use_count(), owners);
  participant = Participant{};
  destroyOnSessionThread(session);
}

// A bare proxy, as in SingleThread mode, must not lock the session from another thread.
TEST_F(SessionParticipantTest, LockingOffTheSessionThreadFailsInDebug) {
  if (!folly::kIsDebug) {
    GTEST_SKIP() << "the check is debug-only";
  }
  testing::FLAGS_gtest_death_test_style = "threadsafe";
  auto session = makeSession();
  Participant participant;
  onSessionThread([&] {
    participant = SessionParticipant::makeParticipant(session, /*relayExec=*/nullptr);
  });
  EXPECT_DEBUG_DEATH(
      (void)folly::coro::blockingWait(
          participant.publisher->subscribe(makeSubscribe(), std::make_shared<MockTrackConsumer>())
      ),
      "session locked off its executor"
  );
  participant = Participant{};
  destroyOnSessionThread(session);
}

} // namespace
