/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <folly/SocketAddress.h>
#include <moxygen/MoQRelaySession.h>

namespace openmoq::moqx {

namespace logging {
class AccessLogSessionContext;
}

// A session accepted on one of the relay's listeners. Carries the listener's name and, once SETUP
// succeeds, the name of the service it was bound to.
class MoqxRelaySession : public moxygen::MoQRelaySession {
public:
  template <typename... Args>
  explicit MoqxRelaySession(std::string listener, Args&&... args)
      : moxygen::MoQRelaySession(std::forward<Args>(args)...), listener_(std::move(listener)) {}

  // Records the local address that the peer connected to, before wt moves into the session.
  template <typename... Args>
  static std::shared_ptr<MoqxRelaySession>
  make(std::string listener, folly::MaybeManagedPtr<proxygen::WebTransport> wt, Args&&... args) {
    auto local = wt ? wt->getLocalAddress() : folly::SocketAddress();
    auto session = std::make_shared<MoqxRelaySession>(
        std::move(listener),
        std::move(wt),
        std::forward<Args>(args)...
    );
    session->localAddress_ = std::move(local);
    return session;
  }

  // Empty for outbound connections.
  static std::string_view listenerOf(const moxygen::MoQSession& session) {
    const auto* accepted = dynamic_cast<const MoqxRelaySession*>(&session);
    return accepted ? std::string_view(accepted->listener_) : std::string_view();
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

  // Records the service chosen at SETUP. Only the first call has effect.
  // Call on the session's executor.
  void bindService(std::string_view service) {
    if (service_.empty()) {
      service_ = service;
    }
  }

  // The service recorded at SETUP; empty for failed SETUP, outbound sessions, and sessions that
  // are not MoqxRelaySession. Call on the session's executor.
  static std::string_view serviceOf(const moxygen::MoQSession& session) {
    const auto* accepted = dynamic_cast<const MoqxRelaySession*>(&session);
    return accepted ? std::string_view(accepted->service_) : std::string_view();
  }

private:
  std::string listener_;
  std::shared_ptr<logging::AccessLogSessionContext> accessLogContext_;
  folly::SocketAddress localAddress_;
  std::string service_;
};

} // namespace openmoq::moqx
