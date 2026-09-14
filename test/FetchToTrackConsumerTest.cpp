/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/FetchToTrackConsumer.h"
#include "TestUtils.h"

#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <moxygen/test/Mocks.h>

using namespace testing;

namespace openmoq::moqx::test {
using namespace moxygen; // NOLINT: bring moxygen protocol types into scope

namespace {
std::shared_ptr<NiceMock<MockSubgroupConsumer>> makeSubgroup() {
  auto sg = std::make_shared<NiceMock<MockSubgroupConsumer>>();
  ON_CALL(*sg, object(_, _, _, _)).WillByDefault(Return(folly::unit));
  ON_CALL(*sg, endOfGroup(_)).WillByDefault(Return(folly::unit));
  ON_CALL(*sg, endOfSubgroup()).WillByDefault(Return(folly::unit));
  return sg;
}
} // namespace

// Flat fetch output is regrouped onto one subgroup stream per (group, subgroup),
// and every stream the adapter opened is closed when the fetch ends.
TEST(FetchToTrackConsumerTest, OpensOneSubgroupPerGroupAndClosesThemAtEndOfFetch) {
  auto track = std::make_shared<NiceMock<MockTrackConsumer>>();
  auto sg1 = makeSubgroup();
  auto sg2 = makeSubgroup();
  EXPECT_CALL(*track, beginSubgroup(1, 0, _, _))
      .WillOnce(Return(std::shared_ptr<SubgroupConsumer>(sg1)));
  EXPECT_CALL(*track, beginSubgroup(2, 0, _, _))
      .WillOnce(Return(std::shared_ptr<SubgroupConsumer>(sg2)));
  {
    InSequence seq;
    EXPECT_CALL(*sg1, object(0, _, _, _)).WillOnce(Return(folly::unit));
    EXPECT_CALL(*sg1, object(1, _, _, _)).WillOnce(Return(folly::unit));
  }
  EXPECT_CALL(*sg2, object(0, _, _, _)).WillOnce(Return(folly::unit));
  EXPECT_CALL(*sg1, endOfSubgroup()).WillOnce(Return(folly::unit));
  EXPECT_CALL(*sg2, endOfSubgroup()).WillOnce(Return(folly::unit));

  FetchToTrackConsumer adapter(track, kDefaultPriority);
  EXPECT_TRUE(adapter.object(1, 0, 0, makeBuf(10)).hasValue());
  EXPECT_TRUE(adapter.object(1, 0, 1, makeBuf(10)).hasValue());
  EXPECT_TRUE(adapter.object(2, 0, 0, makeBuf(10)).hasValue());
  EXPECT_TRUE(adapter.endOfFetch().hasValue());
}

// A group-end marker closes that group's stream with endOfGroup rather than a
// bare endOfSubgroup, so the subscriber learns the group is complete.
TEST(FetchToTrackConsumerTest, EndOfGroupClosesOnlyThatGroupsStream) {
  auto track = std::make_shared<NiceMock<MockTrackConsumer>>();
  auto sg1 = makeSubgroup();
  auto sg2 = makeSubgroup();
  EXPECT_CALL(*track, beginSubgroup(1, 0, _, _))
      .WillOnce(Return(std::shared_ptr<SubgroupConsumer>(sg1)));
  EXPECT_CALL(*track, beginSubgroup(2, 0, _, _))
      .WillOnce(Return(std::shared_ptr<SubgroupConsumer>(sg2)));
  EXPECT_CALL(*sg1, endOfGroup(1)).WillOnce(Return(folly::unit));
  EXPECT_CALL(*sg1, endOfSubgroup()).Times(0);
  EXPECT_CALL(*sg2, endOfSubgroup()).WillOnce(Return(folly::unit));

  FetchToTrackConsumer adapter(track, kDefaultPriority);
  EXPECT_TRUE(adapter.object(1, 0, 0, makeBuf(10)).hasValue());
  EXPECT_TRUE(adapter.endOfGroup(1, 0, 1).hasValue());
  EXPECT_TRUE(adapter.object(2, 0, 0, makeBuf(10)).hasValue());
  EXPECT_TRUE(adapter.endOfFetch().hasValue());
}

} // namespace openmoq::moqx::test
