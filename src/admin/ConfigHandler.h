/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>

#include "config/Config.h"

namespace openmoq::moqx::admin {

class AdminServer;

// Registers GET /config on the given admin server.
//
// Dumps the resolved, active configuration as JSON: listeners (with merged
// quic/mvfst tunables), services, admin, and top-level relay settings.
// HMAC signing secrets are redacted — key ids are shown, secret values are
// replaced with "<redacted>". The snapshot is captured at registration time
// and never changes (moqx has no runtime config reload).
void registerConfigRoute(AdminServer& adminServer, std::shared_ptr<const config::Config> config);

} // namespace openmoq::moqx::admin
