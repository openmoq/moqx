#include "o_rly/relay.h"

int main() {
  o_rly::Relay relay;
  return relay.Version().empty() ? 1 : 0;
}
