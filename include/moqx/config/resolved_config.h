#pragma once

#include <string>
#include <vector>

#include "moqx/config/config.h"

namespace openmoq::moqx::config {

struct ResolvedConfig {
  Config config;
  std::vector<std::string> warnings;
};

} // namespace openmoq::moqx::config
