/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/FetchOkGate.h"

#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <moxygen/test/Mocks.h>

using namespace moxygen;
using openmoq::moqx::FetchOkGate;
using testing::_;
using testing::Return;
using testing::StrictMock;

namespace {

folly::Expected<folly::Unit, MoQPublishError> ok() {
  return folly::unit;
}

class FetchOkGateTest : public testing::Test {
protected:
  std::shared_ptr<StrictMock<MockFetchConsumer>> downstream_{
      std::make_shared<StrictMock<MockFetchConsumer>>()
  };
  FetchOkGate gate_{downstream_};
};

} // namespace

TEST_F(FetchOkGateTest, FinOnObjectHeldUntilAccept) {
  EXPECT_CALL(*downstream_, object(0, 0, 0, _, _, /*finFetch=*/false, _)).WillOnce(Return(ok()));
  EXPECT_TRUE(gate_.object(0, 0, 0, nullptr, noExtensions(), true, false).hasValue());
  EXPECT_TRUE(gate_.hasPendingTerminal());

  EXPECT_CALL(*downstream_, endOfFetch()).WillOnce(Return(ok()));
  gate_.accept();
  EXPECT_FALSE(gate_.hasPendingTerminal());
}

TEST_F(FetchOkGateTest, EndOfTrackHeldUntilAccept) {
  EXPECT_TRUE(gate_.endOfTrackAndGroup(1, 0, 5).hasValue());
  EXPECT_TRUE(gate_.hasPendingTerminal());

  EXPECT_CALL(*downstream_, endOfTrackAndGroup(1, 0, 5)).WillOnce(Return(ok()));
  gate_.accept();
}

TEST_F(FetchOkGateTest, ResetDropsHeldFinAndResetsOnce) {
  EXPECT_TRUE(gate_.endOfFetch().hasValue());

  EXPECT_CALL(*downstream_, reset(ResetStreamErrorCode::INTERNAL_ERROR));
  gate_.reset(ResetStreamErrorCode::INTERNAL_ERROR);
  gate_.reset(ResetStreamErrorCode::CANCELLED);
  EXPECT_TRUE(gate_.wasReset());
}

TEST_F(FetchOkGateTest, PassesThroughAfterAccept) {
  gate_.accept();
  EXPECT_CALL(*downstream_, object(0, 0, 0, _, _, /*finFetch=*/true, _)).WillOnce(Return(ok()));
  EXPECT_TRUE(gate_.object(0, 0, 0, nullptr, noExtensions(), true, false).hasValue());
  EXPECT_FALSE(gate_.hasPendingTerminal());
}
