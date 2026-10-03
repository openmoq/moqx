/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <string_view>
#include <utility>

#include <moxygen/MoQRelaySession.h>

namespace openmoq::moqx {

// A session accepted on one of the relay's listeners. Carries the listener's name.
class MoqxRelaySession : public moxygen::MoQRelaySession {
public:
  template <typename... Args>
  explicit MoqxRelaySession(std::string listener, Args&&... args)
      : moxygen::MoQRelaySession(std::forward<Args>(args)...), listener_(std::move(listener)) {}

  // Empty for a session that was not accepted by a listener (an upstream connection).
  static std::string_view listenerOf(const moxygen::MoQSession& session) {
    const auto* accepted = dynamic_cast<const MoqxRelaySession*>(&session);
    return accepted ? std::string_view(accepted->listener_) : std::string_view();
  }

private:
  std::string listener_;
};

} // namespace openmoq::moqx
