/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>

#include <moxygen/MoQConsumers.h>
#include <moxygen/Publisher.h>
#include <moxygen/Subscriber.h>

#include "logging/access/AccessLogRequest.h"

namespace openmoq::moqx::logging {

// Wraps a request's handle, consumer or callback to write its end lines, REQUEST_UPDATE lines
// and messages. sessionHeld marks the wrapper that the session keeps. When the session
// releases it, the request is finished. Returns nullptr for a null inner, except for the
// namespace and blocked handles, which the session ignores when null.
template <typename Interface>
std::shared_ptr<Interface> logged(
    std::shared_ptr<Interface> inner,
    std::shared_ptr<AccessLogRequest> logRequest,
    bool sessionHeld
);

// Writes the PUBLISH transaction line when the reply completes. A reply task destroyed before
// it runs writes it as aborted.
folly::coro::Task<folly::Expected<moxygen::PublishOk, moxygen::PublishError>> logPublishReply(
    folly::coro::Task<folly::Expected<moxygen::PublishOk, moxygen::PublishError>> reply,
    std::shared_ptr<AccessLogRequest> logRequest
);

} // namespace openmoq::moqx::logging
