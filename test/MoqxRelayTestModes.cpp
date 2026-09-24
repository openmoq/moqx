/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MoqxRelayTestFixture.h"

namespace openmoq::moqx::test {

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

} // namespace openmoq::moqx::test
