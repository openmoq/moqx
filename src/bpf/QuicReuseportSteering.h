/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

namespace openmoq::moqx {

// Enable or disable BPF reuseport steering at runtime.
// Must be called before the QUIC server starts binding sockets.
#ifdef MOQX_ENABLE_BPF_STEERING
void quicReuseportSetEnabled(bool enabled);
#else
inline void quicReuseportSetEnabled(bool) {}
#endif

} // namespace openmoq::moqx
