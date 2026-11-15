/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "config/Config.h"

#include <fizz/server/FizzServerContext.h>

#include <memory>
#include <string>
#include <vector>

namespace openmoq::moqx::tls {

// Server TLS context for a listener: the built-in dev cert (insecure), PKCS#12
// material, or PEM files. Throws std::runtime_error naming the files when the
// PEM cert/key cannot be loaded.
std::shared_ptr<const fizz::server::FizzServerContext>
buildFizzServerContext(const config::TlsMode& tlsMode, const std::vector<std::string>& alpns);

} // namespace openmoq::moqx::tls
