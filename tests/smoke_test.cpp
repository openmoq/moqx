#include "o_rly/relay.h"

int main() {
  openmoq::o_rly::Relay relay;
  return relay.Version().empty() ? 1 : 0;
}
