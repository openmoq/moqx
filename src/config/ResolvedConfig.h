/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <vector>

#include "config/Config.h"

namespace openmoq::moqx::config {

// NOLINTNEXTLINE(bugprone-exception-escape)
struct ResolvedConfig {
  Config config;
  std::vector<std::string> warnings;
};

} // namespace openmoq::moqx::config
