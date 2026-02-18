#pragma once

#include <memory>
#include <string>

#include <moxygen/relay/MoQRelay.h>

namespace openmoq::o_rly {

class Relay {
 public:
  Relay();

  std::string Version() const;

 private:
  std::shared_ptr<moxygen::MoQRelay> moqRelay_;
};

}  // namespace openmoq::o_rly
