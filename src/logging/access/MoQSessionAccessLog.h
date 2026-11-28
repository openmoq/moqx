/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <optional>
#include <string>

#include <folly/SocketAddress.h>
#include <moxygen/MoQSession.h>

#include "logging/access/AccessLogSessionContext.h"

namespace openmoq::moqx::logging {

// The session's authority, path and negotiated version, with its service and peer class.
AccessLogSessionSetup
accessLogSetup(const moxygen::MoQSession& session, std::string service, std::string peerClass);

// The session's ID, peer address and EventBase, with a probe that does not keep it alive.
AccessLogSessionSource accessLogSource(
    const std::shared_ptr<moxygen::MoQSession>& session,
    std::string upstream = {},
    const folly::SocketAddress& local = {}
);

// The SESSION_END error code: the error that closed the session, if it has closed.
std::optional<uint64_t> accessLogCloseCode(const moxygen::MoQSession& session);

} // namespace openmoq::moqx::logging
