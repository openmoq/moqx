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

struct ResolvedConfig {
  Config config;
  std::vector<std::string> warnings;
  // True when the value was generated rather than set in the file.
  bool relayIdGenerated;
  bool hopIdGenerated;
};

} // namespace openmoq::moqx::config
