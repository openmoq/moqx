/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

namespace openmoq::moqx {

// Enable or disable BPF reuseport steering at runtime.
// Must be called before the QUIC server starts binding sockets.
// Has no effect when MOQX_ENABLE_BPF_STEERING is not set at build time.
void quicReuseportSetEnabled(bool enabled);

} // namespace openmoq::moqx
