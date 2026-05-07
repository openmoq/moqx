/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>

namespace openmoq::moqx {
class MoqxRelayContext;
} // namespace openmoq::moqx

namespace openmoq::moqx::admin {

class AdminServer;

// Registers POST /cache/purge on the given admin server.
//
// Accepts an optional JSON body:
//   {"service":"...","namespace":"...","track":"..."}
// All fields are optional; omitting all purges every track across all services.
// Tracks are evicted unconditionally — live writebacks will discover the
// evicted flag and stop caching while continuing to forward data downstream.
// Responds with:
//   {"evicted":<N>,"skipped":<M>}
void registerCachePurgeRoute(AdminServer& adminServer, std::shared_ptr<MoqxRelayContext> context);

} // namespace openmoq::moqx::admin
