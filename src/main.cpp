#include <iostream>

#include "o_rly/relay.h"

int main() {
  o_rly::Relay relay;
  std::cout << "o-rly relay version " << relay.Version() << "\n";
  return 0;
}
