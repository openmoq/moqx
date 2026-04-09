/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>

namespace openmoq::moqx {

class MoqxRelayContext;

namespace admin {
class AdminServer;

// Registers GET /state on the given admin server.
//
// The handler runs dumpState() on the relay worker EVB via co_withExecutor,
// then serializes the result to JSON and sends it to the client.
void registerStateRoute(AdminServer& adminServer, std::shared_ptr<MoqxRelayContext> context);

} // namespace admin

} // namespace openmoq::moqx
