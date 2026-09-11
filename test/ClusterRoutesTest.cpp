/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/ClusterRoutes.h"
#include <gtest/gtest.h>

using namespace openmoq::moqx;

TEST(ClusterRoutesTest, RanksCostThenLengthThenLatestReceipt) {
  ClusterRoutes routes;
  routes.update(1, {7, 10}, 5, 1);
  routes.update(2, {7, 11, 12}, 1, 1);
  ASSERT_EQ(routes.select()->id, 2);
  routes.update(3, {7, 13}, 1, 1);
  ASSERT_EQ(routes.select()->id, 3);
  routes.update(1, {7, 10}, 1, 1);
  EXPECT_EQ(routes.select()->id, 1);
  EXPECT_EQ(routes.size(), 3);
}

TEST(ClusterRoutesTest, SelectsStandbyForExcludedPeerAndZeroExcludesNothing) {
  ClusterRoutes routes;
  routes.update(1, {7, 10}, 0, 1);
  routes.update(2, {7, 0, 11}, 2, 1);
  ASSERT_EQ(routes.select(10)->id, 2);
  ASSERT_EQ(routes.select(0)->id, 1);
  EXPECT_EQ(routes.select(7), nullptr);
  EXPECT_EQ(routes.select(0, 1)->id, 2);
}

TEST(ClusterRoutesTest, ReplacesContentWhenOriginChangesOrIsUnknown) {
  ClusterRoutes routes;
  routes.update(1, {7}, 0, 1);
  auto epoch = routes.contentEpoch();
  EXPECT_FALSE(routes.update(2, {7, 10}, 0, 1));
  EXPECT_EQ(routes.contentEpoch(), epoch);
  EXPECT_TRUE(routes.update(3, {8}, 99, 1));
  EXPECT_EQ(routes.size(), 1);
  EXPECT_GT(routes.contentEpoch(), epoch);
  EXPECT_EQ(routes.select()->id, 3);
  routes.update(4, {0}, 0, 1);
  epoch = routes.contentEpoch();
  EXPECT_TRUE(routes.update(5, {0}, 0, 1));
  EXPECT_GT(routes.contentEpoch(), epoch);
  EXPECT_EQ(routes.size(), 1);
}

TEST(ClusterRoutesTest, SameStreamUpdateChargesLinkOnceAndSurvivesOldWithdrawal) {
  ClusterRoutes routes;
  routes.update(1, {7, 10}, 2, 3);
  routes.update(1, {7, 10}, 4, 3);
  EXPECT_EQ(routes.select()->cost, 7);
  routes.update(2, {7, 11}, 6, 3);
  EXPECT_TRUE(routes.remove(1));
  EXPECT_FALSE(routes.remove(1));
  ASSERT_NE(routes.select(), nullptr);
  EXPECT_EQ(routes.select()->id, 2);
}

TEST(ClusterRoutesTest, SaturatesFullWidthCostsAndPreservesFreeLinks) {
  ClusterRoutes routes;
  routes.update(1, {7}, UINT64_MAX - 2, 3);
  routes.update(2, {7, 10}, 0, 0);
  EXPECT_EQ(routes.find(1)->cost, UINT64_MAX);
  EXPECT_EQ(routes.select()->cost, 0);
  EXPECT_EQ(routes.select()->id, 2);
}

TEST(ClusterRoutesTest, ValidatesPathsWithoutTreatingAnonymousHopsAsLoops) {
  EXPECT_TRUE(clusterPathValid({0, 7, 0}));
  EXPECT_FALSE(clusterPathValid({}));
  EXPECT_FALSE(clusterPathValid({7, 0, 7}));
  EXPECT_FALSE(clusterPathContains({0, 7, 0}, 0));
  EXPECT_TRUE(clusterPathContains({0, 7, 0}, 7));
}

TEST(ClusterRoutesTest, WarmDiscountBelongsOnlyToServingPath) {
  ClusterRoutes routes;
  routes.update(1, {7, 10}, 3, 1);
  routes.update(2, {7, 11}, 9, 1);
  EXPECT_EQ(routes.advertisedCost(1, 1, true), 0);
  EXPECT_EQ(routes.advertisedCost(2, 1, true), 10);
  EXPECT_EQ(routes.advertisedCost(1, 1, false), 4);
}

TEST(ClusterRoutesTest, HandoverIsAsymmetricAndEqualIdentitiesStayPut) {
  EXPECT_EQ(clusterHandoverKey("a", 7), UINT64_C(0xe75717faab2e8139));
  EXPECT_NE(clusterHandoverAllowed("a", 7, 8), clusterHandoverAllowed("a", 8, 7));
  EXPECT_FALSE(clusterHandoverAllowed("a", 7, 7));
  EXPECT_FALSE(clusterHandoverAllowed("a", 0, 0));
  EXPECT_FALSE(clusterHandoverAllowed("a", 7, 0));
}

TEST(ClusterRoutesTest, WithdrawalGapPreservesOnlyKnownContentIdentity) {
  ClusterRoutes routes;
  routes.update(1, {7}, 0, 1);
  const auto epoch = routes.contentEpoch();
  routes.remove(1);
  EXPECT_FALSE(routes.update(2, {7}, 0, 1));
  EXPECT_EQ(routes.contentEpoch(), epoch);
  routes.remove(2);
  EXPECT_TRUE(routes.update(3, {8}, 0, 1));
  EXPECT_GT(routes.contentEpoch(), epoch);
  routes.remove(3);
  EXPECT_TRUE(routes.update(4, {0}, 0, 1));
  const auto anonymousEpoch = routes.contentEpoch();
  routes.remove(4);
  EXPECT_TRUE(routes.update(5, {0}, 0, 1));
  EXPECT_GT(routes.contentEpoch(), anonymousEpoch);
}
