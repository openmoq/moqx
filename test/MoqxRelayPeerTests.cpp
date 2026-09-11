/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Originally from github.com/facebookexperimental/moxygen.
 * See the moxygen LICENSE for the original license terms:
 * https://github.com/openmoq/moxygen/blob/main/LICENSE
 *
 * Copyright (c) OpenMOQ contributors.
 */

#include "MoqxRelayTestFixture.h"
#include "UpstreamProvider.h"

namespace openmoq::moqx::test {

// Test: makeNamespaceBridgeHandle routes namespaceMsg to doPublishNamespace
TEST_P(MoQRelayTest, NamespaceBridgeHandleForwardsNamespaceMsg) {
  auto peerSession = createMockSession();

  // Bridge handle routes NAMESPACE messages from peerSession into the relay.
  // For peering the subscription prefix is empty, so the suffix == full namespace.
  auto handle = makeNamespaceBridgeHandle(relay_, peerSession);
  handle->namespaceMsg(kTestNamespace);

  verifyOnRelayExec([&] {
    auto sessions = relay_->findPublishNamespaceSessions(kTestNamespace);
    EXPECT_EQ(sessions.size(), 1u);
    if (!sessions.empty()) {
      EXPECT_EQ(sessions[0], peerSession);
    }
  });

  removeSession(peerSession);
}

// Test: makeNamespaceBridgeHandle routes namespaceDoneMsg to doPublishNamespaceDone
TEST_P(MoQRelayTest, NamespaceBridgeHandleForwardsDoneMsg) {
  auto peerSession = createMockSession();
  auto handle = makeNamespaceBridgeHandle(relay_, peerSession);

  handle->namespaceMsg(kTestNamespace);
  verifyOnRelayExec([&] {
    EXPECT_EQ(relay_->findPublishNamespaceSessions(kTestNamespace).size(), 1u);
  });

  handle->namespaceDoneMsg(kTestNamespace);
  verifyOnRelayExec([&] {
    EXPECT_TRUE(relay_->findPublishNamespaceSessions(kTestNamespace).empty());
  });

  removeSession(peerSession);
}

// A reconnect changes the session identity but retains the negotiated hop ID.
// Advertisements whose path contains that hop must remain excluded.
TEST_P(MoQRelayTest, PeerNamespaceNotEchoedBackOnReconnect) {
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "sg-sin-2-1");
  relay_->setAllowedNamespacePrefix(kAllowedPrefix);

  // Step 1: session1 is the peer's old (unreaped) connection.  Inject NS as
  // if it arrived from peer "jp-osa-1" via the bridge handle.
  auto session1 = createMockSession();
  auto bridgeHandle = makeNamespaceBridgeHandle(relay_, session1, "jp-osa-1");
  ON_CALL(*session1, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft18)));
  ON_CALL(*session1, negotiatedSetupExtension(SetupExtension::RelayHops))
      .WillByDefault(Return(true));
  ON_CALL(*session1, getPeerHopID()).WillByDefault(Return(uint64_t{123}));
  Namespace announcement{kTestNamespace};
  announcement.params.insertParam(Parameter(
      folly::to_underlying(TrackRequestParamKey::HOP_PATH),
      encodeRelayHopPath({7, 123}, kVersionDraft18).value()
  ));
  bridgeHandle->namespaceMsg(std::move(announcement));
  verifyOnRelayExec([&] {
    EXPECT_EQ(relay_->findPublishNamespaceSessions(kTestNamespace).size(), 1u);
  });

  // Step 2: jp-osa-1 reconnects as session2 and sends a peer SUBSCRIBE_NAMESPACE.
  auto session2 = createMockSession();
  ON_CALL(*session2, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft18)));
  ON_CALL(*session2, negotiatedSetupExtension(SetupExtension::RelayHops))
      .WillByDefault(Return(true));
  ON_CALL(*session2, getPeerHopID()).WillByDefault(Return(uint64_t{123}));

  // Detect any namespace delivered back to the same cluster hop.
  auto nsHandle = std::make_shared<NiceMock<MockNamespacePublishHandle>>();
  bool echoedBack = false;
  ON_CALL(*nsHandle, namespaceMsg(_)).WillByDefault([&echoedBack](const TrackNamespace&) {
    echoedBack = true;
  });

  withSessionContext(session2, [&]() {
    // makePeerSubNs("jp-osa-1") carries jp-osa-1's relay ID in the auth token.
    auto task = publisherInterface()->subscribeNamespace(makePeerSubNs("jp-osa-1"), nsHandle);
    folly::coro::blockingWait(std::move(task), exec_.get());
  });

  EXPECT_FALSE(echoedBack) << "Relay echoed a peer's own namespace back to it on reconnect";

  removeSession(session1);
  removeSession(session2);
}

// Complement: namespaces from LOCAL publishers (not from the peer) must still
// be delivered when that peer subscribes.
TEST_P(MoQRelayTest, LocalNamespaceDeliveredToPeerOnReconnect) {
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "sg-sin-2-1");
  relay_->setAllowedNamespacePrefix(kAllowedPrefix);

  // Local publisher session announces kTestNamespace.
  auto localPublisher = createMockSession();
  doPublishNamespace(localPublisher, kTestNamespace);

  // Peer "jp-osa-1" subscribes with a draft-16 session.
  auto peerSession = createMockSession();
  ON_CALL(*peerSession, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft16)));

  auto nsHandle = std::make_shared<NiceMock<MockNamespacePublishHandle>>();
  bool delivered = false;
  ON_CALL(*nsHandle, namespaceMsg(_)).WillByDefault([&delivered](const TrackNamespace&) {
    delivered = true;
  });

  withSessionContext(peerSession, [&]() {
    auto task = publisherInterface()->subscribeNamespace(makePeerSubNs("jp-osa-1"), nsHandle);
    folly::coro::blockingWait(std::move(task), exec_.get());
  });
  EXPECT_TRUE(delivered) << "Relay failed to deliver a local namespace to a peer subscriber";

  removeSession(localPublisher);
  removeSession(peerSession);
  driveIfMultiThread(); // flush relay cleanup so it drops session refs before mocks are destroyed
}

// A mock session that simulates a peer announcing peerNs when the relay
// calls subscribeNamespace on it (the reciprocal leg).
//
// handleOut receives the bridge handle so the test can control its lifetime
// independently of the session object — mirroring how SubNsStreamCallback
// lives in a coroutine frame that is separate from (but tied to) the session.
class PeerAnnounceSession : public NiceMock<MockMoQSession> {
public:
  explicit PeerAnnounceSession(
      std::shared_ptr<MoQExecutor> exec,
      TrackNamespace peerNs,
      std::shared_ptr<Publisher::NamespacePublishHandle>& handleOut
  )
      : NiceMock<MockMoQSession>(std::move(exec)), peerNs_(std::move(peerNs)),
        handleOut_(handleOut) {}

  folly::coro::Task<Publisher::SubscribeNamespaceResult> subscribeNamespace(
      SubscribeNamespace subNs,
      std::shared_ptr<Publisher::NamespacePublishHandle> handle
  ) override {
    handleOut_ = handle;
    if (handle) {
      Namespace announcement{peerNs_};
      announcement.params.insertParam(Parameter(
          folly::to_underlying(TrackRequestParamKey::HOP_PATH),
          encodeRelayHopPath({7, 123}, kVersionDraft18).value()
      ));
      handle->namespaceMsg(std::move(announcement));
    }
    co_return std::make_shared<NiceMock<MockSubscribeNamespaceHandle>>(
        SubscribeNamespaceOk{subNs.requestID}
    );
  }

private:
  TrackNamespace peerNs_;
  std::shared_ptr<Publisher::NamespacePublishHandle>& handleOut_;
};

// Exercise reciprocal namespace subscription and reconnect exclusion together.
TEST_P(MoQRelayTest, PeerNamespaceNotEchoedBack_FullProductionPath) {
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "sg-sin-2-1");
  relay_->setAllowedNamespacePrefix(kAllowedPrefix);

  // Step 1: jp-osa-1 connects as session1.  It will announce kTestNamespace
  // to us via the reciprocal bridge handle when we subscribe to it.
  // bridgeHandle1 is held here (not inside the session) to model the production
  // lifetime: the handle lives in the coroutine frame, separate from the session.
  std::shared_ptr<Publisher::NamespacePublishHandle> bridgeHandle1;
  auto session1 = std::make_shared<PeerAnnounceSession>(exec_, kTestNamespace, bridgeHandle1);
  ON_CALL(*session1, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft18)));
  ON_CALL(*session1, negotiatedSetupExtension(SetupExtension::RelayHops))
      .WillByDefault(Return(true));
  ON_CALL(*session1, getPeerHopID()).WillByDefault(Return(uint64_t{123}));
  ON_CALL(*session1, getRelayLinkCost()).WillByDefault(Return(uint64_t{1}));
  getOrCreateMockState(session1);

  auto nsHandle1 = std::make_shared<NiceMock<MockNamespacePublishHandle>>();
  withSessionContext(session1, [&]() {
    auto task = publisherInterface()->subscribeNamespace(makePeerSubNs("jp-osa-1"), nsHandle1);
    folly::coro::blockingWait(std::move(task), exec_.get());
  });

  // The reciprocal stream registered the namespace with hop 123 in its path.
  verifyOnRelayExec([&] {
    EXPECT_EQ(relay_->findPublishNamespaceSessions(kTestNamespace).size(), 1u);
  });

  // Step 2: jp-osa-1 reconnects as session2 and re-subscribes.
  // kTestNamespace must NOT be echoed back.
  auto session2 = createMockSession();
  ON_CALL(*session2, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft18)));
  ON_CALL(*session2, negotiatedSetupExtension(SetupExtension::RelayHops))
      .WillByDefault(Return(true));
  ON_CALL(*session2, getPeerHopID()).WillByDefault(Return(uint64_t{123}));

  auto nsHandle2 = std::make_shared<NiceMock<MockNamespacePublishHandle>>();
  bool echoedBack = false;
  ON_CALL(*nsHandle2, namespaceMsg(_)).WillByDefault([&echoedBack](const TrackNamespace&) {
    echoedBack = true;
  });

  withSessionContext(session2, [&]() {
    auto task = publisherInterface()->subscribeNamespace(makePeerSubNs("jp-osa-1"), nsHandle2);
    folly::coro::blockingWait(std::move(task), exec_.get());
  });

  EXPECT_FALSE(echoedBack) << "Relay echoed peer namespace back on reconnect (production path)";

  // Simulate session1's coroutine frame being destroyed before session cleanup.
  bridgeHandle1.reset();
  removeSession(session1);
  removeSession(session2);
}

// Regression test: when a bridge handle is destroyed (ungraceful session close)
// without graceful namespaceDoneMsg calls, tree entries it created must be
// cleaned up so stale sourceSession shared_ptrs don't keep dead session objects
// alive and downstream subscribers receive NAMESPACE_DONE.
TEST_P(MoQRelayTest, BridgeHandleDestructorCleansUpNamespaces) {
  auto upstreamSession = createMockSession();

  // Simulate the bridge path: create a handle and announce a namespace through
  // it, as happens when the relay subscribes to an upstream/peer.
  auto bridgeHandle = makeNamespaceBridgeHandle(relay_, upstreamSession, /*peerID=*/{});
  bridgeHandle->namespaceMsg(kTestNamespace);
  verifyOnRelayExec([&] {
    EXPECT_EQ(relay_->findPublishNamespaceSessions(kTestNamespace).size(), 1u);
  });

  // Drop the bridge handle without graceful namespaceDoneMsg — simulates
  // ungraceful session close destroying SubNsStreamCallback.
  bridgeHandle.reset();

  // Tree entry must be gone.
  verifyOnRelayExec([&] {
    EXPECT_EQ(relay_->findPublishNamespaceSessions(kTestNamespace).size(), 0u)
        << "Stale namespace tree entry persists after bridge handle destruction";
  });

  removeSession(upstreamSession);
}

// Verify that when a new publisher takes over a namespace before the old
// bridge handle is destroyed, the stale handle's destructor does NOT evict
// the new publisher's entry (doPublishNamespaceDone guards on sourceSession).
TEST_P(MoQRelayTest, BridgeHandleDestructorDoesNotEvictNewPublisher) {
  auto session1 = createMockSession();
  auto session2 = createMockSession();

  // session1 announces NS via bridge handle.
  auto bridgeHandle1 = makeNamespaceBridgeHandle(relay_, session1, /*peerID=*/{});
  bridgeHandle1->namespaceMsg(kTestNamespace);
  verifyOnRelayExec([&] {
    EXPECT_EQ(relay_->findPublishNamespaceSessions(kTestNamespace).size(), 1u);
  });

  // session2 takes over NS (conflict path evicts session1, sets sourceSession=session2).
  auto bridgeHandle2 = makeNamespaceBridgeHandle(relay_, session2, /*peerID=*/{});
  bridgeHandle2->namespaceMsg(kTestNamespace);
  verifyOnRelayExec([&] {
    EXPECT_EQ(relay_->findPublishNamespaceSessions(kTestNamespace).size(), 1u);
  });

  // Now session1's handle is destroyed (ungraceful close detected late).
  // It must NOT evict session2's entry.
  bridgeHandle1.reset();

  verifyOnRelayExec([&] {
    EXPECT_EQ(relay_->findPublishNamespaceSessions(kTestNamespace).size(), 1u)
        << "Stale bridge handle destructor evicted the new publisher's entry";
  });

  bridgeHandle2.reset();
  removeSession(session1);
  removeSession(session2);
}

} // namespace openmoq::moqx::test
