/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>

#include <moxygen/Publisher.h>
#include <moxygen/Subscriber.h>

#include "logging/access/AccessLogSessionContext.h"

namespace openmoq::moqx::logging {

// Filters that log the requests passing through them and wrap each request's handles and
// consumers. dir is Rx for the peer's requests and Tx for the relay's. Each returns inner
// when context is null or filtered out. Calls run on the session's executor.
std::shared_ptr<moxygen::Publisher> logPublisher(
    std::shared_ptr<moxygen::Publisher> inner,
    std::shared_ptr<AccessLogSessionContext> context,
    AccessLogDir dir
);

std::shared_ptr<moxygen::Subscriber> logSubscriber(
    std::shared_ptr<moxygen::Subscriber> inner,
    std::shared_ptr<AccessLogSessionContext> context,
    AccessLogDir dir
);

} // namespace openmoq::moqx::logging
