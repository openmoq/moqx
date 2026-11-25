/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <catapult/crypto.hpp>

#include <memory>
#include <string>

namespace openmoq::moqx::auth {

// PEM files, restricted to P-256. The relay loads public material only.
std::unique_ptr<const catapult::CryptographicAlgorithm>
loadEs256PublicKeyFile(const std::string& path);
std::unique_ptr<const catapult::CryptographicAlgorithm>
loadEs256PrivateKeyFile(const std::string& path);

} // namespace openmoq::moqx::auth
