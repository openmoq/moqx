#include "o_rly/relay.h"

namespace openmoq::o_rly {

Relay::Relay() = default;

std::string Relay::Version() const {
  return "0.1.0";
}

}  // namespace openmoq::o_rly
