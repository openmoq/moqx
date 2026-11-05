/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Copyright (c) OpenMOQ contributors.
 * Originally from github.com/facebookexperimental/moxygen.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "NamespaceTree.h"
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <moxygen/MoQSession.h>

using namespace testing;
using namespace moxygen;

namespace openmoq::moqx::test {

class NamespaceTreeTest : public ::testing::Test {
protected:
  NamespaceTree tree_;

  // The tree only uses a participant's identity.
  static Participant makeParticipant() {
    return Participant(
        std::make_shared<const ParticipantInfo>(
            ParticipantInfo{.sessionId = MoQSession::makeSessionId()}
        ),
        nullptr,
        nullptr
    );
  }

  static SessionId idOf(const Participant& participant) { return participant.info->sessionId; }

  std::optional<SessionId>
  publisherOf(const TrackNamespace& ns, const RouteExclusion& exclusion = {}) {
    auto publisher = tree_.findPublisher(ns, exclusion);
    return publisher ? std::optional(idOf(*publisher)) : std::nullopt;
  }

  std::shared_ptr<NamespaceTree::NamespaceNode>
  publish(const TrackNamespace& ns, const Participant& participant) {
    return tree_.setPublisher(ns, participant, nullptr, {}, RequestID(0)).node;
  }

  void unpublish(const TrackNamespace& ns, const Participant& participant) {
    tree_.unpublishNamespace(ns, idOf(participant)); // NodeNotFound / NotOwner are ignorable
  }

  void subscribe(const TrackNamespace& ns, const Participant& participant) {
    tree_.addNamespaceSubscriber(ns, participant, {});
  }

  void unsubscribe(const TrackNamespace& ns, const Participant& participant) {
    tree_.removeNamespaceSubscriber(ns, idOf(participant)); // NotSubscribed is ignorable
  }

  void publishTrack(
      const TrackNamespace& ns,
      const std::string& trackName,
      const Participant& participant
  ) {
    tree_.addPublish({ns, trackName}, participant, {});
  }
};

// Test: Leaf pruning keeps siblings intact
// Scenario: test/A/B/C and test/A/D both exist.
// Removing B/C should prune B (and C) but keep A and D.
TEST_F(NamespaceTreeTest, PruneLeafKeepSiblings) {
  auto publisherABC = makeParticipant();
  auto publisherAD = makeParticipant();

  TrackNamespace nsABC{{"test", "A", "B", "C"}};
  TrackNamespace nsAD{{"test", "A", "D"}};
  publish(nsABC, publisherABC);
  publish(nsAD, publisherAD);

  // Removing B/C should prune B (and C) but keep A and D
  unpublish(nsABC, publisherABC);

  EXPECT_EQ(publisherOf(nsAD), idOf(publisherAD));
}

// Test: SUBSCRIBE routes past a publisher-less node
// Scenario: test/A has a publisher. A subscriber to test/A/B creates a node
// there with no publisher of its own. Looking up test/A/B/C must still find
// test/A's publisher instead of stopping at the publisher-less B.
TEST_F(NamespaceTreeTest, FindPublisherSkipsPublisherlessNode) {
  auto publisher = makeParticipant();

  TrackNamespace nsA{{"test", "A"}};
  TrackNamespace nsAB{{"test", "A", "B"}};
  TrackNamespace nsABC{{"test", "A", "B", "C"}};

  publish(nsA, publisher);
  subscribe(nsAB, makeParticipant());

  EXPECT_EQ(publisherOf(nsABC), idOf(publisher));
}

// Test: Tree pruning removes highest empty ancestor
// Scenario: test/A/B/C only. Remove C should prune A (highest empty after test)
TEST_F(NamespaceTreeTest, PruneHighestEmptyAncestor) {
  auto publisher = makeParticipant();

  TrackNamespace nsABC{{"test", "A", "B", "C"}};
  publish(nsABC, publisher);
  // Remove C — should prune A (highest empty ancestor below root)
  unpublish(nsABC, publisher);

  // The entire subtree under the top-level "test" component should be gone.
  EXPECT_EQ(tree_.findNode(TrackNamespace{{"test"}}), nullptr);
  EXPECT_EQ(tree_.findNode(nsABC), nullptr);

  // Re-publishing must create fresh nodes without crashing.
  auto publisher2 = makeParticipant();
  auto node = publish(nsABC, publisher2);
  EXPECT_NE(node, nullptr);
}

// Test: Pruning happens after explicit unpublish (simulating removeSession)
TEST_F(NamespaceTreeTest, PruneOnUnpublish) {
  auto publisher = makeParticipant();

  TrackNamespace nsABCD{{"test", "A", "B", "C", "D"}};
  publish(nsABCD, publisher);
  unpublish(nsABCD, publisher);

  EXPECT_EQ(tree_.findNode(nsABCD), nullptr);

  // Re-publishing must work after pruning.
  auto publisher2 = makeParticipant();
  auto node = publish(nsABCD, publisher2);
  EXPECT_NE(node, nullptr);
}

// Test: Unpublish from the non-owner is ignored; new owner remains
TEST_F(NamespaceTreeTest, NonOwnerUnpublishIsIgnored) {
  auto publisher1 = makeParticipant();
  auto publisher2 = makeParticipant();

  TrackNamespace nsAB{{"test", "A", "B"}};
  publish(nsAB, publisher1);
  publish(nsAB, publisher2);   // replaces publisher1 as owner
  unpublish(nsAB, publisher1); // NotOwner — should be ignored
  EXPECT_EQ(publisherOf(nsAB), idOf(publisher2));
}

// Test: unpublishTrack prunes node when it was the only content
TEST_F(NamespaceTreeTest, PublishDonePrunesNode) {
  auto publisher = makeParticipant();

  TrackNamespace nsABC{{"test", "A", "B", "C"}};
  auto node = publish(nsABC, publisher);

  // Simulate a track-level publish on the same node (as relay's publish() does)
  publishTrack(nsABC, "track1", publisher);

  // Unpublish namespace — node stays alive because the track publish is still active
  unpublish(nsABC, publisher);
  EXPECT_NE(tree_.findNode(nsABC), nullptr);

  // Unpublish the track — node is now empty, so it should be pruned
  tree_.unpublishTrack(nsABC, "track1");
  EXPECT_EQ(tree_.findNode(nsABC), nullptr);
}

// Test: Mixed content types — node with publisher + active track publish
TEST_F(NamespaceTreeTest, MixedContentPublishAndTrack) {
  auto publisher = makeParticipant();

  TrackNamespace nsAB{{"test", "A", "B"}};
  auto node = publish(nsAB, publisher);

  publishTrack(nsAB, "track1", publisher);

  // Unpublish namespace — node stays alive because track is still there
  unpublish(nsAB, publisher);
  EXPECT_NE(tree_.findNode(nsAB), nullptr);

  // Adding another track still works
  publishTrack(nsAB, "track2", publisher);
  EXPECT_NE(tree_.findNode(nsAB), nullptr);
}

// Test: Mixed content types — node with publisher + namespace subscriber
TEST_F(NamespaceTreeTest, MixedContentPublishAndSubscriber) {
  auto publisher = makeParticipant();
  auto subscriber = makeParticipant();

  TrackNamespace nsAB{{"test", "A", "B"}};
  publish(nsAB, publisher);
  subscribe(nsAB, subscriber); // SUBSCRIBE_NAMESPACE from another session

  // Unpublish namespace — node stays alive because subscriber is still there
  unpublish(nsAB, publisher);
  EXPECT_NE(tree_.findNode(nsAB), nullptr);

  // New publisher can take the namespace
  auto publisher2 = makeParticipant();
  publish(nsAB, publisher2);
  EXPECT_EQ(publisherOf(nsAB), idOf(publisher2));
}

// Test: Unsubscribe triggers pruning when node has no other content
TEST_F(NamespaceTreeTest, PruneOnUnsubscribeNamespace) {
  auto subscriber = makeParticipant();

  TrackNamespace nsABC{{"test", "A", "B", "C"}};
  subscribe(nsABC, subscriber);
  unsubscribe(nsABC, subscriber);

  EXPECT_EQ(tree_.findNode(nsABC), nullptr);

  // Re-subscribing creates a fresh tree without crash
  subscribe(nsABC, subscriber);
  EXPECT_NE(tree_.findNode(nsABC), nullptr);
}

// Test: Middle empty nodes in deep tree
// Scenario: test/A (has publisher), test/A/B (empty), test/A/B/C (has
// publisher). Remove C should prune B but keep A.
TEST_F(NamespaceTreeTest, PruneMiddleEmptyNode) {
  auto publisherA = makeParticipant();
  auto publisherC = makeParticipant();

  TrackNamespace nsA{{"test", "A"}};
  TrackNamespace nsABC{{"test", "A", "B", "C"}};
  publish(nsA, publisherA);
  publish(nsABC, publisherC); // creates B as an empty intermediate node

  // Removing C should prune B and C but keep A
  unpublish(nsABC, publisherC);

  EXPECT_EQ(publisherOf(nsA), idOf(publisherA));

  // Re-publishing nsABC works — B and C were pruned so fresh nodes are created
  auto publisherC2 = makeParticipant();
  EXPECT_NE(publish(nsABC, publisherC2), nullptr);
}

// Test: Double unpublish doesn't crash or corrupt state
TEST_F(NamespaceTreeTest, DoubleUnpublishNamespace) {
  auto publisher = makeParticipant();

  TrackNamespace nsAB{{"test", "A", "B"}};
  publish(nsAB, publisher);
  unpublish(nsAB, publisher);

  EXPECT_EQ(tree_.findNode(nsAB), nullptr);

  unpublish(nsAB, publisher); // NodeNotFound — should not crash

  auto publisher2 = makeParticipant();
  EXPECT_NE(publish(nsAB, publisher2), nullptr);
}

// Test: Unpublish from an unrelated session (child publisher) is also ignored
TEST_F(NamespaceTreeTest, UnrelatedSessionUnpublishIsIgnored) {
  auto publisher1 = makeParticipant();
  auto publisher2 = makeParticipant();

  // publisher1 owns nsAB; publisher2 owns a child namespace
  TrackNamespace nsAB{{"test", "A", "B"}};
  TrackNamespace nsABC{{"test", "A", "B", "C"}};
  publish(nsAB, publisher1);
  publish(nsABC, publisher2);

  // publisher2 tries to unpublish nsAB — NotOwner, ignored
  unpublish(nsAB, publisher2);

  EXPECT_EQ(publisherOf(nsAB), idOf(publisher1))
      << "Ownership check failed: wrong session returned after unrelated unpublish";
}

// Test: Pruning with multiple children at same level
// Scenario: test/A has children B (publisher), C (subscriber), D (subscriber).
// Remove B should prune B but keep A (with children C and D).
TEST_F(NamespaceTreeTest, PruneOneOfMultipleChildren) {
  auto publisherB = makeParticipant();
  auto subscriberC = makeParticipant();
  auto subscriberD = makeParticipant();

  TrackNamespace nsAB{{"test", "A", "B"}};
  TrackNamespace nsAC{{"test", "A", "C"}};
  TrackNamespace nsAD{{"test", "A", "D"}};
  publish(nsAB, publisherB);
  subscribe(nsAC, subscriberC); // creates C as a node with only a subscriber session
  subscribe(nsAD, subscriberD); // creates D similarly

  // Unpublish B — should prune B only, not A (which still has children C and D)
  unpublish(nsAB, publisherB);

  EXPECT_NE(tree_.findNode(nsAC), nullptr);
  EXPECT_NE(tree_.findNode(nsAD), nullptr);
}

// Test: Empty namespace edge case — setPublisher with empty namespace must not crash
TEST_F(NamespaceTreeTest, EmptyNamespacePublish) {
  auto publisher = makeParticipant();

  TrackNamespace emptyNs{{}};
  auto result = tree_.setPublisher(emptyNs, publisher, nullptr, {}, RequestID(0));
  EXPECT_NE(result.node, nullptr);

  unpublish(emptyNs, publisher);
}

// Test: Verify activeChildCount consistency after complex operations
TEST_F(NamespaceTreeTest, ActiveChildCountConsistency) {
  auto pub1 = makeParticipant();
  auto pub2 = makeParticipant();
  auto sub1 = makeParticipant();

  TrackNamespace nsAB{{"test", "A", "B"}};
  TrackNamespace nsAC{{"test", "A", "C"}};
  TrackNamespace nsA{{"test", "A"}};

  publish(nsAB, pub1);
  subscribe(nsAC, sub1);

  unpublish(nsAB, pub1); // removes B; A's activeChildCount drops to 1 (only C)

  // A should still exist because C is still active; pub2 can publish at A
  publish(nsA, pub2);

  unsubscribe(nsAC, sub1); // removes C; A's activeChildCount drops to 0

  // A itself has content (pub2's sourceSession), so it should not be pruned
  EXPECT_EQ(publisherOf(nsA), idOf(pub2));
}

// Test: Track publish keeps node alive while namespace is still published
TEST_F(NamespaceTreeTest, PublishKeepsNodeAliveAfterNamespaceDone) {
  auto publisher = makeParticipant();

  TrackNamespace nsAB{{"test", "A", "B"}};
  auto node = publish(nsAB, publisher);

  publishTrack(nsAB, "track1", publisher);

  // Unpublish namespace (track is still active)
  unpublish(nsAB, publisher);
  EXPECT_NE(tree_.findNode(nsAB), nullptr);

  // Add a second track
  publishTrack(nsAB, "track2", publisher);

  tree_.unpublishTrack(nsAB, "track1");
  EXPECT_NE(tree_.findNode(nsAB), nullptr); // track2 still alive

  tree_.unpublishTrack(nsAB, "track2");
  EXPECT_EQ(tree_.findNode(nsAB), nullptr); // fully pruned
}

// Test: Replacing a namespace's publisher leaves its ancestors prunable
TEST_F(NamespaceTreeTest, ReplacedPublisherKeepsAncestorsPrunable) {
  auto pub1 = makeParticipant();
  auto pub2 = makeParticipant();

  TrackNamespace nsAB{{"test", "A", "B"}};
  TrackNamespace nsA{{"test", "A"}};
  publish(nsAB, pub1);
  publish(nsAB, pub2);
  unpublish(nsAB, pub2);

  EXPECT_EQ(tree_.findNode(nsAB), nullptr);
  EXPECT_EQ(tree_.findNode(nsA), nullptr);
}

class NamespaceTreeRouteTest : public NamespaceTreeTest {
protected:
  NamespaceTree::SetPublisherResult advertise(
      const TrackNamespace& ns,
      const Participant& participant,
      std::vector<uint64_t> path,
      uint64_t cost
  ) {
    return tree_.setPublisher(
        ns,
        participant,
        nullptr,
        {},
        RequestID(0),
        std::move(path),
        cost,
        /*fromCluster=*/true
    );
  }

  const TrackNamespace ns_{{"test", "A"}};
};

// The MoQ Cluster rule: no 0 in the path, then cost, then length, then newest.
TEST_F(NamespaceTreeRouteTest, SelectsByAnonCostLengthThenNewest) {
  auto anonCheap = makeParticipant();
  auto expensive = makeParticipant();
  auto longer = makeParticipant();
  auto older = makeParticipant();
  auto newer = makeParticipant();
  advertise(ns_, anonCheap, {0, 7}, 1);
  advertise(ns_, expensive, {1, 7}, 9);
  EXPECT_EQ(publisherOf(ns_), idOf(expensive));
  advertise(ns_, longer, {1, 2, 7}, 3);
  EXPECT_EQ(publisherOf(ns_), idOf(longer));
  advertise(ns_, older, {1, 7}, 3);
  EXPECT_EQ(publisherOf(ns_), idOf(older));
  advertise(ns_, newer, {2, 7}, 3);
  EXPECT_EQ(publisherOf(ns_), idOf(newer));
  EXPECT_EQ(tree_.findNode(ns_)->routes().size(), 5u);

  std::vector<SessionId> order;
  for (const auto* route : tree_.findNode(ns_)->rankedRoutes()) {
    order.push_back(idOf(route->participant));
  }
  EXPECT_EQ(
      order,
      (std::vector<
          SessionId>{idOf(newer), idOf(older), idOf(longer), idOf(expensive), idOf(anonCheap)})
  );
}

TEST_F(NamespaceTreeRouteTest, PlainRouteReplacesOnlyThePlainRoute) {
  auto cluster = makeParticipant();
  auto plain1 = makeParticipant();
  auto plain2 = makeParticipant();
  advertise(ns_, cluster, {1, 7}, 5);
  EXPECT_EQ(
      tree_.setPublisher(ns_, plain1, nullptr, {}, RequestID(0)).replacedPublisher,
      std::nullopt
  );
  EXPECT_EQ(
      tree_.setPublisher(ns_, plain2, nullptr, {}, RequestID(0)).replacedPublisher,
      idOf(plain1)
  );
  const auto& routes = tree_.findNode(ns_)->routes();
  ASSERT_EQ(routes.size(), 2u);
  EXPECT_EQ(idOf(routes[0].participant), idOf(cluster));
  EXPECT_EQ(idOf(routes[1].participant), idOf(plain2));
}

TEST_F(NamespaceTreeRouteTest, SameSessionReplacesItsRoute) {
  auto peer = makeParticipant();
  advertise(ns_, peer, {1, 7}, 5);
  EXPECT_EQ(advertise(ns_, peer, {2, 7}, 3).replacedPublisher, idOf(peer));
  const auto& routes = tree_.findNode(ns_)->routes();
  ASSERT_EQ(routes.size(), 1u);
  EXPECT_EQ(routes[0].path, (std::vector<uint64_t>{2, 7}));
}

TEST_F(NamespaceTreeRouteTest, UnpublishRemovesOnlyThatSessionsRoute) {
  auto a = makeParticipant();
  auto b = makeParticipant();
  advertise(ns_, a, {1, 7}, 1);
  advertise(ns_, b, {2, 7}, 2);
  auto result = tree_.unpublishNamespace(ns_, idOf(a));
  ASSERT_TRUE(result.hasValue());
  EXPECT_EQ(idOf(result->removed.participant), idOf(a));
  EXPECT_EQ(publisherOf(ns_), idOf(b));
  unpublish(ns_, b);
  EXPECT_EQ(tree_.findNode(ns_), nullptr);
}

// A repeated NAMESPACE keeps the advertisement while its origin holds.
TEST_F(NamespaceTreeRouteTest, RepeatedNamespaceFromTheSameOriginUpdatesInPlace) {
  auto peer = makeParticipant();
  advertise(ns_, peer, {1, 7}, 5);
  const auto seq = tree_.findNode(ns_)->routes()[0].seq;

  EXPECT_EQ(advertise(ns_, peer, {1, 8}, 3).replacedPublisher, std::nullopt);
  auto route = tree_.findNode(ns_)->routes()[0];
  EXPECT_EQ(route.seq, seq);
  EXPECT_EQ(route.path, (std::vector<uint64_t>{1, 8}));
  EXPECT_EQ(route.cost, 3u);

  EXPECT_EQ(advertise(ns_, peer, {2, 8}, 3).replacedPublisher, idOf(peer));
  ASSERT_EQ(tree_.findNode(ns_)->routes().size(), 1u);
  EXPECT_NE(tree_.findNode(ns_)->routes()[0].seq, seq);
}

// A PUBLISH_NAMESPACE is named by its session and request ID, so a stale
// request cannot change or end a newer one.
TEST_F(NamespaceTreeRouteTest, RouteIsNamedBySessionAndRequestID) {
  auto publisher = makeParticipant();
  tree_.setPublisher(ns_, publisher, nullptr, {}, RequestID(4), {1, 7}, 5, /*fromCluster=*/true);
  const auto id = idOf(publisher);
  EXPECT_EQ(
      tree_.updateRoute(ns_, id, RequestID(3), {1, 7}, 1).error(),
      NamespaceTree::Error::NotOwner
  );
  EXPECT_TRUE(tree_.updateRoute(ns_, id, RequestID(4), {1, 7}, 1).hasValue());
  EXPECT_EQ(tree_.findNode(ns_)->routes()[0].cost, 1u);
  EXPECT_EQ(
      tree_.unpublishNamespace(ns_, id, RequestID(3)).error(),
      NamespaceTree::Error::NotOwner
  );
  EXPECT_TRUE(tree_.unpublishNamespace(ns_, id, RequestID(4)).hasValue());
}

// The deepest tier decides: when all of its routes are excluded, a shorter
// prefix does not serve the request.
TEST_F(NamespaceTreeRouteTest, ExclusionDoesNotFallBackToAShorterPrefix) {
  auto parent = makeParticipant();
  auto viaPeer = makeParticipant();
  auto local = makeParticipant();
  advertise(TrackNamespace{{"test"}}, parent, {3, 7}, 1);
  advertise(ns_, viaPeer, {1, 7}, 1);
  advertise(ns_, local, {2, 7}, 5);

  EXPECT_EQ(publisherOf(ns_, {.hop = 1}), idOf(local));
  EXPECT_EQ(publisherOf(ns_, {.session = idOf(viaPeer)}), idOf(local));
  unpublish(ns_, local);
  EXPECT_EQ(publisherOf(ns_, {.hop = 1}), std::nullopt);
}

} // namespace openmoq::moqx::test
