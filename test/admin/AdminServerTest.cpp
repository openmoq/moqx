/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <folly/SocketAddress.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/EventBaseManager.h>

#include "admin/AdminServer.h"

namespace openmoq::moqx::admin {

namespace {

config::AdminConfig adminConfigOn(uint16_t port) {
  config::AdminConfig cfg;
  cfg.address = folly::SocketAddress("127.0.0.1", port);
  return cfg;
}

// Holds a listening TCP socket so a second bind to the same port fails.
class ListeningSocket {
public:
  ListeningSocket() {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    EXPECT_EQ(::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    EXPECT_EQ(::listen(fd_, 1), 0);
    socklen_t len = sizeof(addr);
    EXPECT_EQ(::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len), 0);
    port_ = ntohs(addr.sin_port);
  }
  ~ListeningSocket() { ::close(fd_); }
  ListeningSocket(const ListeningSocket&) = delete;
  ListeningSocket& operator=(const ListeningSocket&) = delete;

  uint16_t port() const { return port_; }

private:
  int fd_{-1};
  uint16_t port_{0};
};

} // namespace

TEST(AdminServerTest, EvbIsNullBeforeStart) {
  AdminServer server;
  EXPECT_EQ(server.evb(), nullptr);
}

// The captured EVB must be the one route handlers see through EventBaseManager,
// since admin views are installed on it.
TEST(AdminServerTest, StartCapturesTheWorkerEventBase) {
  AdminServer server;
  ASSERT_TRUE(server.start(adminConfigOn(0)));

  auto* evb = server.evb();
  ASSERT_NE(evb, nullptr);

  folly::EventBase* seen = nullptr;
  evb->runInEventBaseThreadAndWait([&] { seen = folly::EventBaseManager::get()->getEventBase(); });
  EXPECT_EQ(seen, evb);

  server.stop();
}

TEST(AdminServerTest, StopClearsEventBase) {
  AdminServer server;
  ASSERT_TRUE(server.start(adminConfigOn(0)));
  server.stop();
  EXPECT_EQ(server.evb(), nullptr);
}

TEST(AdminServerTest, FailedStartReturnsFalseWithoutEventBase) {
  ListeningSocket busy;
  AdminServer server;
  EXPECT_FALSE(server.start(adminConfigOn(busy.port())));
  EXPECT_EQ(server.evb(), nullptr);
}

} // namespace openmoq::moqx::admin
