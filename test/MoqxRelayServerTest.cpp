/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MoqxRelayServer.h"

#include <folly/coro/BlockingWait.h>
#include <folly/coro/Sleep.h>
#include <folly/io/IOBuf.h>
#include <folly/portability/GTest.h>
#include <moxygen/MoQWebTransportClient.h>
#include <moxygen/events/MoQFollyExecutorImpl.h>
#include <moxygen/util/InsecureVerifierDangerousDoNotUseInProduction.h>
#include <moxygen/util/QuicConnector.h>
#include <quic/client/QuicClientTransport.h>
#include <quic/state/StateData.h>

#include <chrono>
#include <memory>
#include <string>

namespace openmoq::moqx::test {
namespace {

class MoqxRelayServerTest : public ::testing::Test {
protected:
  void SetUp() override { startServer(); }

  void startServer(uint64_t h3TxnTimeoutMs = 0) {
    if (server_) {
      server_->stop();
    }
    config::ListenerConfig listener;
    listener.address = folly::SocketAddress("::1", 0);
    listener.tlsMode = config::Insecure{};
    listener.endpoint = "/moq";
    listener.moqtVersions = "18";
    listener.mvfst.h3TxnTimeoutMs = h3TxnTimeoutMs;
    auto context = std::make_shared<MoqxRelayContext>(
        folly::F14FastMap<std::string, config::ServiceConfig>{
            {"default",
             config::ServiceConfig{
                 .match = {{
                     .authority = config::ServiceConfig::MatchEntry::AnyAuthority{},
                     .path = config::ServiceConfig::MatchEntry::PrefixPath{"/"},
                 }},
             }},
        },
        "test-relay"
    );
    server_ = std::make_unique<MoqxRelayServer>(listener, std::move(context), &ioExecutor_);
    server_->start();
  }

  void TearDown() override {
    wtClient_.reset();
    if (client_) {
      client_->closeNow({});
      client_.reset();
    }
    if (server_) {
      server_->stop();
      server_.reset();
    }
    clientEvb_.loop();
  }

  void connect(const std::string& alpn, bool reliableResetSupport) {
    quic::TransportSettings settings;
    settings.advertisedReliableResetStreamSupport = reliableResetSupport;
    client_ = folly::coro::blockingWait(
        moxygen::QuicConnector::connectQuic(
            &clientEvb_,
            server_->getAddress(),
            std::chrono::seconds(5),
            std::make_shared<moxygen::test::InsecureVerifierDangerousDoNotUseInProduction>(),
            {alpn},
            settings
        ),
        &clientEvb_
    );
  }

  void connectWebTransport() {
    wtClient_ = std::make_unique<moxygen::MoQWebTransportClient>(
        std::make_shared<moxygen::MoQFollyExecutorImpl>(&clientEvb_),
        proxygen::URL(
            folly::to<std::string>("https://localhost:", server_->getAddress().getPort(), "/moq")
        ),
        std::make_shared<moxygen::test::InsecureVerifierDangerousDoNotUseInProduction>()
    );
    folly::coro::blockingWait(
        wtClient_->setupMoQSession(
            std::chrono::seconds(5),
            std::chrono::seconds(30),
            nullptr,
            nullptr,
            quic::TransportSettings(),
            moxygen::getMoqtProtocols("18", true)
        ),
        &clientEvb_
    );
  }

  folly::EventBase clientEvb_;
  folly::IOThreadPoolExecutor ioExecutor_{1};
  std::unique_ptr<MoqxRelayServer> server_;
  std::shared_ptr<quic::QuicClientTransport> client_;
  std::unique_ptr<moxygen::MoQWebTransportClient> wtClient_;
};

TEST_F(MoqxRelayServerTest, AdvertisesReliableResetSupportForWebTransport) {
  connect("h3", true);

  ASSERT_NE(client_, nullptr);
  ASSERT_NE(client_->getState(), nullptr);
  EXPECT_TRUE(client_->getState()->peerAdvertisedReliableStreamResetSupport);
}

TEST_F(MoqxRelayServerTest, AcceptsHttp3PeerWithoutReliableResetSupport) {
  connect("h3", false);

  ASSERT_NE(client_, nullptr);
  EXPECT_TRUE(client_->good());
}

TEST_F(MoqxRelayServerTest, ReliableResetKeepsConnectionOpen) {
  connect("h3", true);
  ASSERT_NE(client_, nullptr);
  ASSERT_TRUE(client_->getState()->peerAdvertisedReliableStreamResetSupport);

  auto stream = client_->createUnidirectionalStream();
  ASSERT_TRUE(stream.has_value());
  // Reserved HTTP/3 stream type 0x21, which the server must ignore.
  ASSERT_TRUE(client_->writeChain(*stream, folly::IOBuf::copyBuffer("\x21reliable"), false));
  ASSERT_TRUE(client_->updateReliableDeliveryCheckpoint(*stream));
  ASSERT_TRUE(client_->writeChain(*stream, folly::IOBuf::copyBuffer("unreliable"), false));
  ASSERT_TRUE(client_->resetStreamReliably(*stream, quic::ApplicationErrorCode(0x10c)));

  folly::coro::blockingWait(folly::coro::sleep(std::chrono::seconds(1)), &clientEvb_);

  EXPECT_TRUE(client_->good());
  EXPECT_FALSE(client_->getState()->localConnectionError.has_value());
  EXPECT_FALSE(client_->getState()->peerConnectionError.has_value());
}

TEST_F(MoqxRelayServerTest, H3TxnTimeoutEndsIdleWebTransportSession) {
  startServer(/*h3TxnTimeoutMs=*/1000);
  connectWebTransport();
  auto session = wtClient_->moqSession_;
  ASSERT_NE(session, nullptr);
  ASSERT_FALSE(session->isClosed());

  // Deadline under the 30 s QUIC and client timeouts and the 60 s default.
  auto start = std::chrono::steady_clock::now();
  while (!session->isClosed() && std::chrono::steady_clock::now() < start + std::chrono::seconds(10)
  ) {
    folly::coro::blockingWait(folly::coro::sleep(std::chrono::milliseconds(50)), &clientEvb_);
  }
  EXPECT_TRUE(session->isClosed());
  EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(500));
}

} // namespace
} // namespace openmoq::moqx::test
