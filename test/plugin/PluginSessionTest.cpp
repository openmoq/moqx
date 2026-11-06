/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

// A real relay server and client, with the test plugin in the service chain:
// what reaches the wire while admit and authorize suspend.

#include "MoqxRelayServer.h"
#include "plugin/TestPlugin.h"

#include <folly/coro/BlockingWait.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <moxygen/MoQClient.h>
#include <moxygen/MoQRelaySession.h>
#include <moxygen/events/MoQFollyExecutorImpl.h>
#include <moxygen/test/Mocks.h>
#include <moxygen/util/InsecureVerifierDangerousDoNotUseInProduction.h>

#include <chrono>
#include <thread>

namespace openmoq::moqx::plugin::test {
namespace {

using namespace moxygen;
using ::testing::IsEmpty;
using ::testing::NiceMock;
using ::testing::SizeIs;

class PluginSessionTest : public ::testing::Test {
protected:
  void SetUp() override {
    currentScript() = std::make_shared<Script>();
    script_ = currentScript();
  }

  void start() {
    config::ServiceConfig svc;
    svc.match = {config::ServiceConfig::MatchEntry{
        .authority = config::ServiceConfig::MatchEntry::AnyAuthority{},
        .path = config::ServiceConfig::MatchEntry::PrefixPath{"/"},
    }};
    TestConfig plugin;
    plugin.name = "gate";
    svc.plugins = {config::PluginEntryConfig{
        .type = "test",
        .id = "gate",
        .rank = 0,
        .pluginType = testType(),
        .config = std::make_shared<const TestConfig>(plugin),
    }};
    config::Config cfg;
    cfg.services.emplace("live", svc);
    host_ = PluginHost::create(cfg).value();

    context_ = std::make_shared<MoqxRelayContext>(
        cfg.services,
        "test-relay",
        /*useRelayThread=*/true,
        /*useLocalForwarders=*/false,
        config::ClusterConfig{},
        host_
    );
    ASSERT_TRUE(host_
                    ->createServices([this](std::string_view service) {
                      return folly::getKeepAliveToken(context_->relayExecutor(service));
                    })
                    .hasValue());

    config::ListenerConfig listener;
    listener.address = folly::SocketAddress("::1", 0);
    listener.tlsMode = config::Insecure{};
    listener.endpoint = "/moq";
    listener.moqtVersions = "18";
    server_ = std::make_shared<MoqxRelayServer>(listener, context_, &ioExecutor_);
    server_->start();

    client_ = std::make_unique<MoQClient>(
        clientExec_,
        proxygen::URL(
            folly::to<std::string>("moqt://localhost:", server_->getAddress().getPort(), "/moq")
        ),
        MoQRelaySession::createRelaySessionFactory(),
        std::make_shared<moxygen::test::InsecureVerifierDangerousDoNotUseInProduction>()
    );
    folly::coro::blockingWait(
        client_->setupMoQSession(
            std::chrono::seconds(2),
            std::chrono::seconds(5),
            nullptr,
            nullptr,
            quic::TransportSettings(),
            getMoqtProtocols("18", /*useStandard=*/true)
        ),
        &clientEvb_
    );
  }

  void TearDown() override {
    script_->admitGate.post();
    script_->authorizeGate.post();
    if (client_ && client_->moqSession_) {
      client_->moqSession_->close(SessionCloseErrorCode::NO_ERROR);
    }
    pump(std::chrono::milliseconds(500));
    client_.reset();
    if (server_) {
      server_->stop();
    }
    clientEvb_.loop();
    if (host_) {
      folly::coro::blockingWait(host_->stop());
    }
  }

  // Runs the client event base for about `duration`.
  void pump(std::chrono::milliseconds duration) {
    auto until = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < until) {
      clientEvb_.loopOnce(EVLOOP_NONBLOCK);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  template <class T> folly::SemiFuture<T> spawn(folly::coro::Task<T> task) {
    return folly::coro::co_withExecutor(&clientEvb_, std::move(task)).start();
  }

  template <class T> T await(folly::SemiFuture<T> future) {
    return std::move(future).via(&clientEvb_).getVia(&clientEvb_);
  }

  size_t authorizedCount() { return script_->authorized.rlock()->size(); }

  SubscribeRequest subscribeRequest() {
    return SubscribeRequest::make(FullTrackName{TrackNamespace{{"live", "room"}}, "video"});
  }

  PublishRequest publishRequest() {
    PublishRequest pub;
    pub.fullTrackName = FullTrackName{TrackNamespace{{"live", "room"}}, "audio"};
    pub.trackAlias = TrackAlias(1);
    pub.groupOrder = GroupOrder::Default;
    pub.forward = true;
    return pub;
  }

  std::shared_ptr<SubscriptionHandle> publishHandle() {
    SubscribeOk ok;
    ok.requestID = RequestID(0);
    ok.trackAlias = TrackAlias(0);
    ok.expires = std::chrono::milliseconds(0);
    ok.groupOrder = GroupOrder::Default;
    return std::make_shared<NiceMock<MockSubscriptionHandle>>(std::move(ok));
  }

  std::shared_ptr<Script> script_;
  folly::IOThreadPoolExecutor ioExecutor_{1};
  folly::EventBase clientEvb_;
  std::shared_ptr<MoQFollyExecutorImpl> clientExec_ =
      std::make_shared<MoQFollyExecutorImpl>(&clientEvb_);
  std::shared_ptr<PluginHost> host_;
  std::shared_ptr<MoqxRelayContext> context_;
  std::shared_ptr<MoqxRelayServer> server_;
  std::unique_ptr<MoQClient> client_;
};

TEST_F(PluginSessionTest, RequestWaitsForAdmission) {
  script_->suspendAdmit = true;
  script_->authorizeErrors.wlock()->emplace("gate", Error{Error::Kind::Forbidden, "denied"});
  start();
  auto consumer = std::make_shared<NiceMock<MockTrackConsumer>>();
  auto subscribed = spawn(client_->moqSession_->subscribe(subscribeRequest(), consumer));
  pump(std::chrono::milliseconds(100));
  // The SUBSCRIBE sits in the transport until admission resolves.
  EXPECT_EQ(authorizedCount(), 0);
  EXPECT_FALSE(subscribed.isReady());

  script_->admitGate.post();
  auto result = await(std::move(subscribed));
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error().errorCode, RequestErrorCode::UNAUTHORIZED);
  EXPECT_EQ(result.error().reasonPhrase, "denied");
  EXPECT_THAT(*script_->authorized.rlock(), ::testing::ElementsAre("gate:subscribe:video:-:gate"));
}

TEST_F(PluginSessionTest, SuspendedPublishRejectionKeepsItsCode) {
  script_->suspendAuthorize = true;
  script_->authorizeErrors.wlock()->emplace("gate", Error{Error::Kind::Unauthorized, "no"});
  start();
  auto published = client_->moqSession_->publish(publishRequest(), publishHandle());
  ASSERT_TRUE(published.hasValue());
  auto reply = spawn(std::move(published->reply));
  pump(std::chrono::milliseconds(50));
  EXPECT_EQ(authorizedCount(), 1);
  EXPECT_FALSE(reply.isReady());

  script_->authorizeGate.post();
  auto result = await(std::move(reply));
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error().errorCode, RequestErrorCode::UNAUTHORIZED);
  EXPECT_EQ(result.error().reasonPhrase, "no");
}

TEST_F(PluginSessionTest, SuspendedPublishAcceptedReachesTheRelay) {
  script_->suspendAuthorize = true;
  start();
  auto published = client_->moqSession_->publish(publishRequest(), publishHandle());
  ASSERT_TRUE(published.hasValue());
  auto reply = spawn(std::move(published->reply));
  pump(std::chrono::milliseconds(50));
  EXPECT_FALSE(reply.isReady());

  script_->authorizeGate.post();
  auto result = await(std::move(reply));
  EXPECT_TRUE(result.hasValue()) << result.error().reasonPhrase;
}

TEST_F(PluginSessionTest, InlinePublishIsAccepted) {
  start();
  auto published = client_->moqSession_->publish(publishRequest(), publishHandle());
  ASSERT_TRUE(published.hasValue());
  auto result = await(spawn(std::move(published->reply)));
  EXPECT_TRUE(result.hasValue()) << result.error().reasonPhrase;
  EXPECT_EQ(authorizedCount(), 1);
}

TEST_F(PluginSessionTest, SessionCloseRunsSessionEnded) {
  script_->hooks = Hook::Admit | Hook::SessionEnded;
  start();
  // The client finishes setup on the proactive SERVER_SETUP; admission runs
  // when CLIENT_SETUP reaches the server.
  for (int i = 0; i < 1000 && script_->events.rlock()->empty(); ++i) {
    pump(std::chrono::milliseconds(5));
  }
  client_->moqSession_->close(SessionCloseErrorCode::NO_ERROR);
  // sessionEnded runs on the server's worker once the session ends there.
  for (int i = 0; i < 1000 && script_->events.rlock()->size() < 2; ++i) {
    pump(std::chrono::milliseconds(5));
  }
  EXPECT_THAT(
      *script_->events.rlock(),
      ::testing::ElementsAre("admit:gate@live", "ended:gate:gate")
  );
}

} // namespace
} // namespace openmoq::moqx::plugin::test
