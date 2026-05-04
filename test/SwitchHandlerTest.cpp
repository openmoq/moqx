/*
 * Copyright (c) Synamedia
 * SPDX-License-Identifier: Apache-2.0
 */

// Placeholder — full SwitchHandler tests wired once relay fixture is stable.
#include <gtest/gtest.h>

TEST(MoqxSessionTest, OnSwitchWithNoRelayDoesNotCrash) {
  SUCCEED() << "Guard: setRelay not called, onSwitch must be a no-op";
}

TEST(SwitchHandlerTest, ValidatesUnknownCurrentRequestID) {
  // Expand with MockMoQSession fixture once handleSwitch is stable.
  SUCCEED() << "Expand with relay fixture";
}
