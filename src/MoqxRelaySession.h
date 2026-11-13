/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <utility>

#include <folly/SocketAddress.h>
#include <moxygen/MoQRelaySession.h>

namespace openmoq::moqx {

namespace logging {
class AccessLogSessionContext;
}

// A session accepted on one of the relay's listeners. It carries moqx's own per-session state.
class MoqxRelaySession : public moxygen::MoQRelaySession {
public:
  using moxygen::MoQRelaySession::MoQRelaySession;

  // Records the local address that the peer connected to, before wt moves into the session.
  template <typename... Args>
  static std::shared_ptr<MoqxRelaySession>
  make(folly::MaybeManagedPtr<proxygen::WebTransport> wt, Args&&... args) {
    auto local = wt ? wt->getLocalAddress() : folly::SocketAddress();
    auto session = std::make_shared<MoqxRelaySession>(std::move(wt), std::forward<Args>(args)...);
    session->localAddress_ = std::move(local);
    return session;
  }

  const folly::SocketAddress& localAddress() const { return localAddress_; }

  // Null for a session that was not accepted by a listener (an upstream connection), and
  // when the access log is off.
  static std::shared_ptr<logging::AccessLogSessionContext>
  accessLogContextOf(const moxygen::MoQSession& session) {
    const auto* accepted = dynamic_cast<const MoqxRelaySession*>(&session);
    return accepted ? accepted->accessLogContext_ : nullptr;
  }

  void setAccessLogContext(std::shared_ptr<logging::AccessLogSessionContext> context) {
    accessLogContext_ = std::move(context);
  }

private:
  std::shared_ptr<logging::AccessLogSessionContext> accessLogContext_;
  folly::SocketAddress localAddress_;
};

} // namespace openmoq::moqx
