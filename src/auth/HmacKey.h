/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace openmoq::moqx::auth {

// Shared by the relay's verifier and the issuer so the HKDF salt/info can't
// drift out of sync (a mismatch yields tokens the relay silently rejects).
std::vector<uint8_t> deriveHmacKey(std::string_view secret);

} // namespace openmoq::moqx::auth
