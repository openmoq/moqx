#include "o_rly/relay.h"

namespace openmoq::o_rly {

Relay::Relay() : moqRelay_(std::make_shared<moxygen::MoQRelay>()) {}

std::string Relay::Version() const {
  return "0.1.0";
}

}  // namespace openmoq::o_rly
