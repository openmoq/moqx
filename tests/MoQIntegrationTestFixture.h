/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/io/async/EventBase.h>
#include <folly/portability/GTest.h>
#include <folly/logging/xlog.h>
#include <folly/synchronization/Baton.h>
#include <moxygen/MoQClient.h>
#include <moxygen/MoQRelaySession.h>
#include <moxygen/MoQServer.h>
#include <moxygen/MoQVersions.h>
#include <moxygen/Publisher.h>
#include <moxygen/Subscriber.h>
#include <moxygen/events/MoQFollyExecutorImpl.h>
#include <moxygen/util/InsecureVerifierDangerousDoNotUseInProduction.h>
#include <proxygen/httpserver/samples/hq/FizzContext.h>
#include <proxygen/lib/utils/URL.h>

#include <thread>

namespace openmoq::moqx::test {

/**
 * MoQIntegrationTestFixture - Reusable GTest fixture that runs a real MoQ
 * server in-process and provides helpers to create real MoQ clients.
 *
 * Subclasses override createServerPublishHandler() and optionally
 * createServerSubscribeHandler() to provide the server-side handlers.
 */
class MoQIntegrationTestFixture : public ::testing::Test {
 protected:
  // Subclasses must override to provide server's Publisher handler
  virtual std::shared_ptr<moxygen::Publisher> createServerPublishHandler() = 0;

  // Subclasses optionally override to provide server's Subscriber handler
  virtual std::shared_ptr<moxygen::Subscriber>
  createServerSubscribeHandler() {
    return nullptr;
  }

  void SetUp() override {
    // Start the server thread. The server must be constructed and started
    // on the same thread (QuicServer requirement).
    folly::Baton<> serverReady;
    serverThread_ = std::thread([this, &serverReady]() {
      XLOG(DBG1) << "Server thread starting";

      // Create insecure fizz context
      auto alpns = moxygen::getDefaultMoqtProtocols(true);
      alpns.push_back("h3");
      auto fizzContext =
          quic::samples::createFizzServerContextWithInsecureDefault(
              alpns,
              fizz::server::ClientAuthMode::None,
              "" /* cert */,
              "" /* key */);

      // Create and start the server on this thread
      server_ = std::make_unique<TestServer>(*this, std::move(fizzContext));
      server_->start(folly::SocketAddress("::1", 0));

      // Get the assigned port
      auto addr = server_->getAddress();
      port_ = addr.getPort();
      XLOG(DBG1) << "Test server started on port " << port_;

      serverReady.post();

      // Run event loop (server creates its own worker EVBs)
      serverEvb_.loopForever();
      XLOG(DBG1) << "Server thread exiting";
    });

    serverReady.wait();
    XLOG(DBG1) << "Server ready on port " << port_;

    // Create client executor - the EVB will be driven by blockingWait
    // in the test thread (not a separate thread)
    clientExec_ = std::make_shared<moxygen::MoQFollyExecutorImpl>(&clientEvb_);
  }

  void TearDown() override {
    // Stop the server
    if (server_) {
      server_->stop();
    }

    // Drain the client EventBase so any pending close/cleanup work can run
    // before tearing down the server.
    clientEvb_.loop();

    // Stop event bases
    serverEvb_.terminateLoopSoon();

    if (serverThread_.joinable()) {
      serverThread_.join();
    }

    server_.reset();
  }

  // Create a URL pointing at the test server
  proxygen::URL serverUrl() const {
    return proxygen::URL(
        folly::to<std::string>("moqt://localhost:", port_, "/test"));
  }

  // Create a new MoQ client configured for the test server
  std::unique_ptr<moxygen::MoQClient> createClient() {
    return std::make_unique<moxygen::MoQClient>(
        clientExec_,
        serverUrl(),
        moxygen::MoQRelaySession::createRelaySessionFactory(),
        std::make_shared<
            moxygen::test::InsecureVerifierDangerousDoNotUseInProduction>());
  }

  // Get the client executor
  std::shared_ptr<moxygen::MoQExecutor> clientExec() const {
    return clientExec_;
  }

  // Get the server's assigned port
  uint16_t serverPort() const {
    return port_;
  }

  // Access the client event base for scheduling
  folly::EventBase& clientEvb() {
    return clientEvb_;
  }

  // Access the server event base for scheduling
  folly::EventBase& serverEvb() {
    return serverEvb_;
  }

 private:
  // Simple MoQServer subclass that delegates to fixture's handler factories
  class TestServer : public moxygen::MoQServer {
   public:
    TestServer(
        MoQIntegrationTestFixture& fixture,
        std::shared_ptr<const fizz::server::FizzServerContext> fizzContext)
        : MoQServer(std::move(fizzContext), "/test"),
          fixture_(fixture) {}

    void onNewSession(
        std::shared_ptr<moxygen::MoQSession> session) override {
      XLOG(DBG1) << "TestServer: new session " << session.get();
      auto pub = fixture_.createServerPublishHandler();
      auto sub = fixture_.createServerSubscribeHandler();
      if (pub) {
        session->setPublishHandler(pub);
      }
      if (sub) {
        session->setSubscribeHandler(sub);
      }
    }

   protected:
    std::shared_ptr<moxygen::MoQSession> createSession(
        folly::MaybeManagedPtr<proxygen::WebTransport> wt,
        std::shared_ptr<moxygen::MoQExecutor> executor) override {
      return std::make_shared<moxygen::MoQRelaySession>(
          folly::MaybeManagedPtr<proxygen::WebTransport>(std::move(wt)),
          *this,
          std::move(executor));
    }

   private:
    MoQIntegrationTestFixture& fixture_;
  };

  std::unique_ptr<TestServer> server_;
  folly::EventBase serverEvb_;
  folly::EventBase clientEvb_;
  std::thread serverThread_;
  uint16_t port_{0};
  std::shared_ptr<moxygen::MoQFollyExecutorImpl> clientExec_;
};

} // namespace openmoq::moqx::test
