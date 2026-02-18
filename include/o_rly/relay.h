#pragma once

#include <string>

namespace openmoq::o_rly {

class Relay {
 public:
  Relay();

  std::string Version() const;
};

}  // namespace openmoq::o_rly
