/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TracksTree.h"
#include <folly/portability/GTest.h>
#include <moxygen/MoQSession.h>

using namespace moxygen;

namespace openmoq::moqx::test {

namespace {

constexpr auto kTracks = TrackSubscriptionSource::SubscribeTracks;
constexpr auto kNamespace = TrackSubscriptionSource::SubscribeNamespace;

Participant makeParticipant() {
  return Participant(
      std::make_shared<const ParticipantInfo>(
          ParticipantInfo{.sessionId = MoQSession::makeSessionId()}
      ),
      nullptr,
      nullptr
  );
}

std::shared_ptr<PropertyRanking> makeRanking(uint64_t propertyType) {
  return std::make_shared<PropertyRanking>(
      propertyType,
      /*maxDeselected=*/0,
      std::chrono::milliseconds(0),
      std::chrono::milliseconds(0),
      [](const FullTrackName&) { return std::chrono::steady_clock::time_point{}; },
      [](const FullTrackName&, const std::vector<std::pair<Participant, bool>>&) {},
      [](const FullTrackName&, const Participant&, bool) {},
      [](const FullTrackName&, const Participant&) {}
  );
}

std::vector<SessionId> sessionsOf(const TracksTree::SubscriberList& list) {
  std::vector<SessionId> out;
  for (const auto& [participant, _] : list) {
    out.push_back(participant.info->sessionId);
  }
  return out;
}

size_t rankingCount(const TracksTree& tree, const TrackNamespace& ns) {
  size_t count = 0;
  tree.forEachRankingOnPath(ns, [&](uint64_t, const auto&) { ++count; });
  return count;
}

} // namespace

TEST(TracksTreeTest, CoversExactPrefixAndAncestors) {
  TracksTree tree;
  auto root = makeParticipant();
  auto conf = makeParticipant();
  auto room = makeParticipant();
  auto other = makeParticipant();
  tree.addSubscriber(TrackNamespace{}, root, {});
  tree.addSubscriber(TrackNamespace{{"conf"}}, conf, {});
  tree.addSubscriber(TrackNamespace{{"conf", "room1"}}, room, {});
  tree.addSubscriber(TrackNamespace{{"conf", "room2"}}, other, {});

  auto covering = sessionsOf(tree.subscribersCovering(TrackNamespace{{"conf", "room1", "a"}}));
  EXPECT_EQ(covering.size(), 3);
  for (const auto& p : {root, conf, room}) {
    EXPECT_NE(std::find(covering.begin(), covering.end(), p.info->sessionId), covering.end());
  }
  EXPECT_EQ(sessionsOf(tree.subscribersCovering(TrackNamespace{{"other"}})).size(), 1);
}

TEST(TracksTreeTest, RemoveIsPerPrefixAndPrunes) {
  TracksTree tree;
  auto viewer = makeParticipant();
  const TrackNamespace room{{"conf", "room1"}};
  tree.addSubscriber(room, viewer, {});
  EXPECT_FALSE(tree.removeSubscriber(TrackNamespace{{"conf"}}, viewer.info->sessionId, kTracks));
  EXPECT_TRUE(tree.removeSubscriber(room, viewer.info->sessionId, kTracks));
  EXPECT_FALSE(tree.removeSubscriber(room, viewer.info->sessionId, kTracks));
  EXPECT_TRUE(tree.subscribersCovering(room).empty());
}

TEST(TracksTreeTest, OverlapIsPerSession) {
  TracksTree tree;
  auto viewer = makeParticipant();
  auto other = makeParticipant();
  tree.addSubscriber(TrackNamespace{{"conf", "room1"}}, viewer, {});
  const auto session = viewer.info->sessionId;
  EXPECT_TRUE(tree.hasOverlap(TrackNamespace{{"conf"}}, session));
  EXPECT_TRUE(tree.hasOverlap(TrackNamespace{{"conf", "room1"}}, session));
  EXPECT_TRUE(tree.hasOverlap(TrackNamespace{{"conf", "room1", "a"}}, session));
  EXPECT_FALSE(tree.hasOverlap(TrackNamespace{{"conf", "room2"}}, session));
  EXPECT_FALSE(tree.hasOverlap(TrackNamespace{{"conf"}}, other.info->sessionId));
}

// A draft-18 session may hold a SUBSCRIBE_NAMESPACE and a SUBSCRIBE_TRACKS at one prefix.
TEST(TracksTreeTest, OneEntryPerSourceAndOverlapCountsOnlyTracks) {
  TracksTree tree;
  auto viewer = makeParticipant();
  const auto session = viewer.info->sessionId;
  const TrackNamespace room{{"conf", "room1"}};
  tree.addSubscriber(room, viewer, {.source = kNamespace});
  EXPECT_FALSE(tree.hasOverlap(room, session));
  tree.addSubscriber(room, viewer, {.source = kTracks});
  EXPECT_TRUE(tree.hasOverlap(room, session));
  EXPECT_EQ(tree.subscribersCovering(room).size(), 2);

  EXPECT_TRUE(tree.removeSubscriber(room, session, kNamespace));
  EXPECT_EQ(tree.subscribersCovering(room).size(), 1);
  EXPECT_TRUE(tree.hasOverlap(room, session));
}

TEST(TracksTreeTest, RepeatedSubscriptionIsRejected) {
  TracksTree tree;
  auto viewer = makeParticipant();
  const TrackNamespace room{{"conf", "room1"}};
  EXPECT_NE(tree.addSubscriber(room, viewer, {.trackFilter = TrackFilter{2, 1}}), nullptr);
  EXPECT_EQ(tree.addSubscriber(room, viewer, {.trackFilter = TrackFilter{2, 5}}), nullptr);
  auto covering = tree.subscribersCovering(room);
  ASSERT_EQ(covering.size(), 1);
  EXPECT_EQ(covering[0].second.trackFilter->maxSelected, 1);
}

TEST(TracksTreeTest, RankingLivesWhileItsNodeHasSubscribers) {
  TracksTree tree;
  auto first = makeParticipant();
  auto second = makeParticipant();
  const TrackNamespace room{{"conf", "room1"}};
  const TrackFilter filter{2, 3};
  auto* node = tree.addSubscriber(room, first, {.trackFilter = filter});
  tree.addSubscriber(room, second, {.trackFilter = filter});
  auto ranking = makeRanking(filter.propertyType);
  tree.ranking(*node, filter.propertyType) = ranking;
  ranking->addSubscriberToTopNGroup(filter.maxSelected, first, true);
  ranking->addSubscriberToTopNGroup(filter.maxSelected, second, true);
  EXPECT_EQ(rankingCount(tree, TrackNamespace{{"conf", "room1", "a"}}), 1);

  tree.removeSubscriber(room, first.info->sessionId, kTracks);
  const auto* group = ranking->getTopNGroup(filter.maxSelected);
  EXPECT_TRUE(group == nullptr || !group->sessions.contains(first.info->sessionId));
  EXPECT_EQ(rankingCount(tree, room), 1);

  tree.removeSubscriber(room, second.info->sessionId, kTracks);
  EXPECT_EQ(rankingCount(tree, room), 0);
}

TEST(TracksTreeTest, RankingSurvivesWhenOnlyDescendantsLeave) {
  TracksTree tree;
  auto viewer = makeParticipant();
  auto deeper = makeParticipant();
  const TrackNamespace conf{{"conf"}};
  auto* node = tree.addSubscriber(conf, viewer, {.trackFilter = TrackFilter{2, 1}});
  tree.ranking(*node, 2) = makeRanking(2);
  tree.addSubscriber(TrackNamespace{{"conf", "room1"}}, deeper, {});
  tree.removeSubscriber(TrackNamespace{{"conf", "room1"}}, deeper.info->sessionId, kTracks);
  EXPECT_EQ(rankingCount(tree, TrackNamespace{{"conf", "room1"}}), 1);
}

} // namespace openmoq::moqx::test
