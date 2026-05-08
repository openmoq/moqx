/*
 * Copyright (c) Synamedia
 * SPDX-License-Identifier: Apache-2.0
 */

#include "switch/GroupStartObserver.h"
#include "switch/SwitchTypes.h"
#include <gtest/gtest.h>

using namespace openmoq::moqx;
using namespace moxygen;

// ---- SwitchTypes ----

TEST(SwitchTypesTest, ParameterKeyValue) {
  EXPECT_EQ(kSwitchTransitionParamKey, 0xFF01u);
}

TEST(SwitchTypesTest, SwitchTransitionFields) {
  SwitchTransition st{7, 12};
  EXPECT_EQ(st.switchingGroupID, 7u);
  EXPECT_EQ(st.liveEdgeGroupID, 12u);
}

// ---- GroupStartObserver ----

TEST(GroupStartObserverTest, CallbackFiresOnBeginSubgroup) {
  std::vector<uint64_t> observed;
  GroupStartObserver obs([&](uint64_t g) { observed.push_back(g); });

  obs.beginSubgroup(5, 0, Priority(0));
  obs.beginSubgroup(6, 0, Priority(0));
  obs.beginSubgroup(6, 1, Priority(0)); // same group, different subgroup

  ASSERT_EQ(observed.size(), 3u);
  EXPECT_EQ(observed[0], 5u);
  EXPECT_EQ(observed[1], 6u);
  EXPECT_EQ(observed[2], 6u);
}

TEST(GroupStartObserverTest, NoopMethodsReturnUnit) {
  GroupStartObserver obs([](uint64_t) {});

  auto sg = obs.beginSubgroup(0, 0, Priority(0));
  ASSERT_TRUE(sg.hasValue());

  EXPECT_TRUE(obs.setTrackAlias(TrackAlias(1)).hasValue());
  EXPECT_TRUE(obs.awaitStreamCredit().hasValue());
  EXPECT_TRUE(
      obs.objectStream(ObjectHeader(0, 0, 0, std::nullopt, ObjectStatus::NORMAL, {}, std::nullopt),
                       nullptr,
                       false)
          .hasValue());
  EXPECT_TRUE(
      obs.datagram(ObjectHeader(0, 0, 0, std::nullopt, ObjectStatus::NORMAL, {}, std::nullopt),
                   nullptr,
                   false)
          .hasValue());
  EXPECT_TRUE(obs.publishDone(PublishDone{}).hasValue());
}

TEST(GroupStartObserverTest, NoopSubgroupMethodsReturnUnit) {
  GroupStartObserver obs([](uint64_t) {});
  auto sgResult = obs.beginSubgroup(3, 0, Priority(0));
  ASSERT_TRUE(sgResult.hasValue());
  auto& sg = *sgResult.value();

  EXPECT_TRUE(sg.object(0, nullptr, {}, false).hasValue());
  EXPECT_TRUE(sg.endOfGroup(0).hasValue());
  EXPECT_TRUE(sg.endOfTrackAndGroup(0).hasValue());
  EXPECT_TRUE(sg.endOfSubgroup().hasValue());
  sg.reset(ResetStreamErrorCode(0)); // must not crash
}
