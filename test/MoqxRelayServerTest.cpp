/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MoqxRelayServer.h"

#include <folly/coro/BlockingWait.h>
#include <folly/portability/GTest.h>
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
  void SetUp() override {
    config::ListenerConfig listener;
    listener.address = folly::SocketAddress("::1", 0);
    listener.tlsMode = config::Insecure{};
    listener.endpoint = "/moq";
    listener.moqtVersions = "18";
    auto context = std::make_shared<MoqxRelayContext>(
        folly::F14FastMap<std::string, config::ServiceConfig>{},
        "test-relay"
    );
    server_ = std::make_unique<MoqxRelayServer>(listener, std::move(context), &ioExecutor_);
    server_->start();
  }

  void TearDown() override {
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

  folly::EventBase clientEvb_;
  folly::IOThreadPoolExecutor ioExecutor_{1};
  std::unique_ptr<MoqxRelayServer> server_;
  std::shared_ptr<quic::QuicClientTransport> client_;
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

} // namespace
} // namespace openmoq::moqx::test
