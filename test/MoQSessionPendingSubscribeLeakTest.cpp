/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Repro for a moxygen MoQSession state leak.
//
// - A locally-initiated SUBSCRIBE inserts the track into
//   pendingSubscribeTracks_.
// - A SUBSCRIBE_OK / SUBSCRIBE_ERROR reply erases it.
// - A peer close of the request stream (FIN or RST) before any reply goes
//   through MoQSession::failPendingRequestOnEarlyClose, which erases only
//   pendingRequests_.
// - The track then stays in pendingSubscribeTracks_ until session teardown,
//   and every later incoming PUBLISH of it on the same session is rejected
//   DUPLICATE_SUBSCRIPTION.
//
// Wire shape (draft 18+, per-request bidi streams):
//   client SUBSCRIBE(T) -> server holds the request, never replies
//   server closes the request stream (FIN, or RST in the second case)
//   client subscribe() fails INTERNAL_ERROR              (correct)
//   server PUBLISH(T) -> must be accepted                (leak: rejected
//                                                         DUPLICATE_SUBSCRIPTION)
//
// moxygen's Draft18Test.SubscribeFailsOnPeerFinWithoutReply covers the
// early-close half; the fix and this PUBLISH-after-close half belong next to
// it upstream. This copy exists because the prebuilt moxygen package ships
// only the mocks, not the upstream session-test fixture.

#include <folly/ScopeGuard.h>
#include <folly/coro/Baton.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/GtestHelpers.h>
#include <folly/coro/Task.h>
#include <folly/io/async/EventBase.h>
#include <folly/logging/xlog.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <moxygen/MoQRelaySession.h>
#include <moxygen/MoQVersions.h>
#include <moxygen/events/MoQFollyExecutorImpl.h>
#include <moxygen/test/Mocks.h>
#include <proxygen/lib/http/webtransport/test/FakeSharedWebTransport.h>

namespace {

using namespace moxygen;
using testing::_;

// Inert under draft 18 (request limits come from QUIC stream limits); sent
// only for wire-shape parity with the upstream fixture.
constexpr uint64_t kMaxRequestID = 100;
const FullTrackName kTrackName{TrackNamespace{{"test"}}, "leak-track"};

SubscribeRequest makeSubscribe() {
  return SubscribeRequest{
      RequestID(0),
      kTrackName,
      0,
      GroupOrder::OldestFirst,
      true,
      LocationType::LargestObject,
      std::nullopt,
      0
  };
}

class TimeoutGuard : public folly::HHWheelTimer::Callback {
public:
  void timeoutExpired() noexcept override { XLOG(FATAL) << "test hung (10s timeout)"; }
};

std::shared_ptr<testing::NiceMock<MockTrackConsumer>> makeNiceTrackConsumer() {
  auto consumer = std::make_shared<testing::NiceMock<MockTrackConsumer>>();
  ON_CALL(*consumer, setTrackAlias(_))
      .WillByDefault(testing::Return(folly::Expected<folly::Unit, MoQPublishError>(folly::unit)));
  ON_CALL(*consumer, publishDone(_)).WillByDefault(testing::Return(folly::unit));
  return consumer;
}

class PendingSubscribeLeakTest : public ::testing::Test, public MoQSession::ServerSetupCallback {
public:
  void SetUp() override {
    eventBase_.timer().scheduleTimeout(&timeout_, std::chrono::seconds(10));
    exec_ = std::make_shared<MoQFollyExecutorImpl>(&eventBase_);
    std::tie(clientWt_, serverWt_) =
        proxygen::test::FakeSharedWebTransport::makeSharedWebTransport();
    clientSession_ = std::make_shared<MoQRelaySession>(
        folly::MaybeManagedPtr<proxygen::WebTransport>(clientWt_.get()),
        exec_
    );
    serverWt_->setPeerHandler(clientSession_.get());
    serverSession_ = std::make_shared<MoQRelaySession>(
        folly::MaybeManagedPtr<proxygen::WebTransport>(serverWt_.get()),
        *this,
        exec_
    );
    clientWt_->setPeerHandler(serverSession_.get());

    // Draft 15+ carries the version in ALPN. Draft 18+ puts each request on
    // its own bidi stream, so a single request can be closed early at all.
    auto alpn = getAlpnFromVersion(kVersionDraft18);
    ASSERT_TRUE(alpn.has_value());
    clientSession_->validateAndSetVersionFromAlpn(*alpn);
    serverSession_->validateAndSetVersionFromAlpn(*alpn);
  }

  void TearDown() override { timeout_.cancelTimeout(); }

  folly::Try<moxygen::Setup>
  onClientSetup(moxygen::Setup /* clientSetup */, const std::shared_ptr<MoQSession>& /* session */)
      override {
    // Draft 18: only success/failure of this callback matters; the reply
    // bytes on the wire come from sendSetup() in setupSessions().
    return folly::Try<moxygen::Setup>(moxygen::Setup{});
  }

  folly::Expected<folly::Unit, SessionCloseErrorCode> validateAuthority(
      const moxygen::Setup& /* clientSetup */,
      uint64_t /* negotiatedVersion */,
      std::shared_ptr<MoQSession> /* session */
  ) override {
    return folly::unit;
  }

protected:
  folly::coro::Task<void> setupSessions() {
    // Each session gets only the handler its incoming requests need: the
    // client receives PUBLISH, the server receives SUBSCRIBE.
    clientSession_->setSubscribeHandler(clientSubscriber_);
    clientSession_->start();
    serverSession_->setPublishHandler(serverPublisher_);
    serverSession_->start();

    // Draft 18+: server proactively sends SERVER_SETUP on its uni control.
    moxygen::Setup serverSetup;
    serverSetup.params.insertParam(
        SetupParameter{folly::to_underlying(SetupKey::MAX_REQUEST_ID), kMaxRequestID}
    );
    serverSession_->sendSetup(std::move(serverSetup));

    moxygen::Setup clientSetup;
    clientSetup.params.insertParam(SetupParameter{folly::to_underlying(SetupKey::PATH), "/test"});
    clientSetup.params.insertParam(
        SetupParameter{folly::to_underlying(SetupKey::MAX_REQUEST_ID), kMaxRequestID}
    );
    co_await clientSession_->setup(std::move(clientSetup));
  }

  // Subscribes to kTrackName, ends the request via closeRequestStream (which
  // must close the server's half of the SUBSCRIBE bidi, stream id 0, without
  // a terminal reply), then requires the follow-up PUBLISH to be accepted.
  folly::coro::Task<void> runEarlyCloseThenPublish(folly::Function<void()> closeRequestStream) {
    co_await setupSessions();

    // The server's publish handler parks the request; no reply is sent.
    folly::coro::Baton serverSawSubscribe;
    folly::coro::Baton releaseHandler;
    // A failed CO_ASSERT below is a bare co_return; the parked handler must
    // still be unblocked and the sessions closed before the frame's batons
    // are destroyed.
    SCOPE_EXIT {
      releaseHandler.post();
      clientSession_->close(SessionCloseErrorCode::NO_ERROR);
      serverSession_->close(SessionCloseErrorCode::NO_ERROR);
    };
    EXPECT_CALL(*serverPublisher_, subscribe(_, _))
        .WillOnce(
            [&](auto sub, auto /* consumer */) -> folly::coro::Task<Publisher::SubscribeResult> {
              serverSawSubscribe.post();
              co_await releaseHandler;
              co_return folly::makeUnexpected(
                  SubscribeError{sub.requestID, SubscribeErrorCode::INTERNAL_ERROR, "test teardown"}
              );
            }
        );

    auto subscribeConsumer = makeNiceTrackConsumer();
    std::optional<SubscribeErrorCode> subscribeErrorCode;
    folly::coro::Baton subscribeDone;
    co_withExecutor(&eventBase_, folly::coro::co_invoke([&, this]() -> folly::coro::Task<void> {
      // co_awaitTry + SCOPE_EXIT: subscribeDone must post even if subscribe()
      // exits with an exception, or the co_await below hangs into the
      // XLOG(FATAL) timeout with no gtest attribution.
      SCOPE_EXIT {
        subscribeDone.post();
      };
      auto res = co_await folly::coro::co_awaitTry(
          clientSession_->subscribe(makeSubscribe(), subscribeConsumer)
      );
      if (res.hasValue() && res->hasError()) {
        subscribeErrorCode = res->error().errorCode;
      }
    })).start();

    co_await serverSawSubscribe;

    // Close the request stream from the peer with no SUBSCRIBE_OK/ERROR.
    closeRequestStream();

    co_await subscribeDone;
    CO_ASSERT_TRUE(subscribeErrorCode.has_value())
        << "subscribe did not fail with an error code despite early stream close";
    EXPECT_EQ(*subscribeErrorCode, SubscribeErrorCode::INTERNAL_ERROR);

    // The peer now publishes the same track on the same session. A request
    // that ended by early close must clean up like one that ended by
    // REQUEST_ERROR, so this PUBLISH must be accepted.
    co_await publishSameTrackExpectAccepted();
  }

  // Server PUBLISHes kTrackName; the client must deliver it to the
  // subscriber application and reply PUBLISH_OK.
  folly::coro::Task<void> publishSameTrackExpectAccepted() {
    bool publishDelivered = false;
    auto publishConsumer = makeNiceTrackConsumer();
    // AtMost: with the leak, the PUBLISH never reaches the application; the
    // reply assertion below is the failure signal, not a missed mock call.
    EXPECT_CALL(*clientSubscriber_, publish(_, _))
        .Times(testing::AtMost(1))
        .WillRepeatedly([&](PublishRequest pub, auto /* handle */) -> Subscriber::PublishResult {
          publishDelivered = true;
          return Subscriber::PublishConsumerAndReplyTask{
              std::static_pointer_cast<TrackConsumer>(publishConsumer),
              folly::coro::makeTask<folly::Expected<PublishOk, PublishError>>(PublishOk{
                  pub.requestID,
                  true,
                  128,
                  GroupOrder::Default,
                  LocationType::LargestObject,
                  std::nullopt,
                  std::make_optional(uint64_t(0))
              })
          };
        });

    auto publishHandle = std::make_shared<MockSubscriptionHandle>(SubscribeOk{
        RequestID(0),
        TrackAlias(100),
        std::chrono::milliseconds(0),
        GroupOrder::Default,
        std::nullopt
    });
    // publish() assigns requestID and trackAlias itself.
    PublishRequest pub;
    pub.fullTrackName = kTrackName;
    pub.largest = AbsoluteLocation{0, 100};
    pub.forward = true;
    auto publishResult = serverSession_->publish(std::move(pub), publishHandle);
    CO_ASSERT_TRUE(publishResult.hasValue());

    auto replyRes = co_await std::move(publishResult.value().reply);
    if (replyRes.hasError()) {
      ADD_FAILURE() << "PUBLISH rejected: code=" << folly::to_underlying(replyRes.error().errorCode)
                    << " reason=" << replyRes.error().reasonPhrase
                    << (replyRes.error().errorCode == PublishErrorCode::DUPLICATE_SUBSCRIPTION
                            ? " (track leaked in pendingSubscribeTracks_)"
                            : "");
    } else {
      EXPECT_TRUE(publishDelivered)
          << "PUBLISH_OK received but the request never reached the subscriber application";
    }
  }

  folly::EventBase eventBase_;
  TimeoutGuard timeout_;
  std::shared_ptr<MoQFollyExecutorImpl> exec_;
  std::unique_ptr<proxygen::test::FakeSharedWebTransport> clientWt_;
  std::unique_ptr<proxygen::test::FakeSharedWebTransport> serverWt_;
  std::shared_ptr<MoQSession> clientSession_;
  std::shared_ptr<MoQSession> serverSession_;
  std::shared_ptr<MockPublisher> serverPublisher_{std::make_shared<MockPublisher>()};
  std::shared_ptr<MockSubscriber> clientSubscriber_{std::make_shared<MockSubscriber>()};
};

TEST_F(PendingSubscribeLeakTest, PublishAfterSubscribeStreamFin) {
  folly::coro::blockingWait(
      runEarlyCloseThenPublish([this] {
        // Server FINs its half of the SUBSCRIBE bidi (client-initiated
        // stream id 0) with no terminal reply.
        serverWt_->writeHandles.at(0)->writeStreamData(nullptr, /*fin=*/true, nullptr);
      }),
      &eventBase_
  );
}

TEST_F(PendingSubscribeLeakTest, PublishAfterSubscribeStreamReset) {
  folly::coro::blockingWait(
      runEarlyCloseThenPublish([this] {
        // Server RSTs its half of the SUBSCRIBE bidi with no terminal reply.
        serverWt_->writeHandles.at(0)->resetStream(
            folly::to_underlying(ResetStreamErrorCode::CANCELLED)
        );
      }),
      &eventBase_
  );
}

// Control: a SUBSCRIBE that ends with a well-formed SUBSCRIBE_ERROR reply
// cleans up pendingSubscribeTracks_, and the same PUBLISH is accepted.
// Shows the early-close cases above fail only for lack of that cleanup.
TEST_F(PendingSubscribeLeakTest, PublishAfterSubscribeErrorReply) {
  folly::coro::blockingWait(
      folly::coro::co_invoke([this]() -> folly::coro::Task<void> {
        co_await setupSessions();

        EXPECT_CALL(*serverPublisher_, subscribe(_, _))
            .WillOnce(
                [](auto sub, auto /* consumer */) -> folly::coro::Task<Publisher::SubscribeResult> {
                  co_return folly::makeUnexpected(
                      SubscribeError{sub.requestID, SubscribeErrorCode::INTERNAL_ERROR, "rejected"}
                  );
                }
            );

        auto res = co_await clientSession_->subscribe(makeSubscribe(), makeNiceTrackConsumer());
        CO_ASSERT_TRUE(res.hasError());
        EXPECT_EQ(res.error().errorCode, SubscribeErrorCode::INTERNAL_ERROR);

        co_await publishSameTrackExpectAccepted();

        clientSession_->close(SessionCloseErrorCode::NO_ERROR);
        serverSession_->close(SessionCloseErrorCode::NO_ERROR);
      }),
      &eventBase_
  );
}

} // namespace
