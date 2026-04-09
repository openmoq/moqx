/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

namespace openmoq::moqx::admin {

class AdminServer;

// Registers built-in admin routes: GET /info, and future /health, /version.
void registerBuiltinRoutes(AdminServer& server);

} // namespace openmoq::moqx::admin
