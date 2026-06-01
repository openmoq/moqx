/*
 * Copyright (c) OpenMOQ contributors.
 */

#include "MoqxRelayTestFixture.h"

namespace moxygen::test {

INSTANTIATE_TEST_SUITE_P(
    AllModes,
    MoQRelayTest,
    ::testing::Values(RelayMode::SingleThread, RelayMode::MultiThread, RelayMode::LocalForwarderMT),
    [](const ::testing::TestParamInfo<RelayMode>& info) -> std::string {
      switch (info.param) {
      case RelayMode::SingleThread:
        return "SingleThread";
      case RelayMode::MultiThread:
        return "MultiThread";
      case RelayMode::LocalForwarderMT:
        return "LocalForwarderMT";
      }
      return "Unknown";
    }
);

} // namespace moxygen::test
