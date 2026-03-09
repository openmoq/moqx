#pragma once

#include <string>
#include <vector>

#include "o_rly/config/config.h"

namespace openmoq::o_rly::config {

struct ResolvedConfig {
  Config config;
  std::vector<std::string> warnings;
};

} // namespace openmoq::o_rly::config
