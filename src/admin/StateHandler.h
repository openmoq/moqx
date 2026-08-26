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
// The handler walks each service on the executor that owns it, writing JSON as
// it goes and sending it chunked as it is produced.
void registerStateRoute(AdminServer& adminServer, std::shared_ptr<MoqxRelayContext> context);

} // namespace admin

} // namespace openmoq::moqx
