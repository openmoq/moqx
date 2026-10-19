/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Copyright (c) OpenMOQ contributors.
 * Originally from github.com/facebookexperimental/moxygen.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MoqxRelayTestFixture.h"
#include "UpstreamProvider.h"

#include <folly/CancellationToken.h>
#include <folly/coro/Baton.h>
#include <folly/coro/Promise.h>
#include <folly/coro/WithCancellation.h>

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

// Regression test: when a peer relay reconnects and subscribes to our
// namespaces, we must not echo back namespaces that originally came FROM that
// peer — doing so overwrites the real publisher in the namespace tree and
// breaks data flow for downstream subscribers.
//
// Scenario (relay acts as sg-sin-2-1, upstream is jp-osa-1):
//   1. jp-osa-1 (session1) announces namespace NS to our relay.
//   2. session1 drops (old QUIC connection not yet reaped).
//   3. jp-osa-1 reconnects (session2) and sends a peer SUBSCRIBE_NAMESPACE.
//   4. Bug: the relay walks its tree, finds NS with sourceSession=session1,
//      session1 != session2, and delivers NS back to session2 — echo loop.
//   5. Fix: NS is tagged with sourcePeerID="jp-osa-1" so the peer ID check
//      suppresses the delivery regardless of session identity.
//
// Production relays negotiate draft-16 (empty prefix allowed).  The delivery
// path for draft-16 is synchronous: namespacePublishHandle->namespaceMsg().
TEST_P(MoQRelayTest, PeerNamespaceNotEchoedBackOnReconnect) {
  // Relay must have a relayID for peer detection to activate.
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "sg-sin-2-1");
  relay_->setAllowedNamespacePrefix(kAllowedPrefix);

  // Step 1: session1 is the peer's old (unreaped) connection.  Inject NS as
  // if it arrived from peer "jp-osa-1" via the bridge handle.
  auto session1 = createMockSession();
  auto bridgeHandle = makeNamespaceBridgeHandle(relay_, session1, "jp-osa-1");
  bridgeHandle->namespaceMsg(kTestNamespace);
  verifyOnRelayExec([&] {
    EXPECT_EQ(relay_->findPublishNamespaceSessions(kTestNamespace).size(), 1u);
  });

  // Step 2: jp-osa-1 reconnects as session2 and sends a peer SUBSCRIBE_NAMESPACE.
  // Peer-to-peer sessions negotiate draft-16 (empty prefix is a 16+ feature).
  auto session2 = createMockSession();
  ON_CALL(*session2, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft16)));

  // The relay delivers existing namespaces via namespacePublishHandle->namespaceMsg
  // (draft-16 synchronous path).  Use a mock handle to detect any echo.
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
      handle->namespaceMsg(peerNs_);
    }
    co_return std::make_shared<NiceMock<MockSubscribeNamespaceHandle>>(
        SubscribeNamespaceOk{subNs.requestID}
    );
  }

private:
  TrackNamespace peerNs_;
  std::shared_ptr<Publisher::NamespacePublishHandle>& handleOut_;
};

// Full production-path regression test: verifies that the peerID stored on
// namespace nodes via the reciprocal bridge handle is the INCOMING peer's relay
// ID (not our own relayID_).
//
// Bug: makeNamespaceBridgeHandle was passed relayID_ ("sg-sin-2-1") instead of
// incomingPeerID ("jp-osa-1"), so nodes got sourcePeerID="sg-sin-2-1".  On
// reconnect the check "sg-sin-2-1" != "jp-osa-1" is true and the namespace
// was echoed back — the loop survived.
//
// Unlike PeerNamespaceNotEchoedBackOnReconnect (which injects the namespace
// directly via makeNamespaceBridgeHandle), this test goes through the full
// publisherInterface()->subscribeNamespace() production path so the bug in the call-site is
// exercised.
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
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft16)));
  getOrCreateMockState(session1);

  auto nsHandle1 = std::make_shared<NiceMock<MockNamespacePublishHandle>>();
  withSessionContext(session1, [&]() {
    auto task = publisherInterface()->subscribeNamespace(makePeerSubNs("jp-osa-1"), nsHandle1);
    folly::coro::blockingWait(std::move(task), exec_.get());
  });

  // kTestNamespace should now be in the tree with sourcePeerID="jp-osa-1".
  verifyOnRelayExec([&] {
    EXPECT_EQ(relay_->findPublishNamespaceSessions(kTestNamespace).size(), 1u);
  });

  // Step 2: jp-osa-1 reconnects as session2 and re-subscribes.
  // kTestNamespace must NOT be echoed back.
  auto session2 = createMockSession();
  ON_CALL(*session2, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft16)));

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

// Answers the reciprocal subscribeNamespace only after fail() is called.
class StalledReciprocalSession : public NiceMock<MockMoQSession> {
public:
  explicit StalledReciprocalSession(std::shared_ptr<MoQExecutor> exec)
      : NiceMock<MockMoQSession>(std::move(exec)) {
    auto [promise, future] = folly::coro::makePromiseContract<void>();
    reply_ = std::move(promise);
    replied_ = std::move(future);
  }

  folly::coro::Task<Publisher::SubscribeNamespaceResult>
  subscribeNamespace(SubscribeNamespace subNs, std::shared_ptr<Publisher::NamespacePublishHandle>)
      override {
    // A closing peer session fails its pending requests. It does not throw.
    co_await folly::coro::co_withCancellation(folly::CancellationToken{}, std::move(replied_));
    co_return folly::makeUnexpected(SubscribeNamespaceError{
        subNs.requestID,
        SubscribeNamespaceErrorCode::INTERNAL_ERROR,
        "Session closed"
    });
  }

  void fail() { reply_.setValue(); }

private:
  folly::coro::Promise<void> reply_;
  folly::coro::Future<void> replied_;
};

// The session is cancelled and removed before its pending reciprocal fails.
// The relay must not register the peer's subscription afterward.
TEST_P(MoQRelayTest, PeerClosedDuringReciprocalIsNotRegistered) {
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "sg-sin-2-1");
  relay_->setAllowedNamespacePrefix(kAllowedPrefix);
  auto peer = std::make_shared<StalledReciprocalSession>(exec_);
  ON_CALL(*peer, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft16)));
  getOrCreateMockState(peer);

  auto nsHandle = std::make_shared<NiceMock<MockNamespacePublishHandle>>();
  std::weak_ptr<Publisher::NamespacePublishHandle> weakHandle = nsHandle;
  folly::CancellationSource cancelSource;
  auto result = withSessionContext(peer, [&]() {
    return folly::coro::co_withExecutor(
               sessionExec(),
               folly::coro::co_withCancellation(
                   cancelSource.getToken(),
                   publisherInterface()
                       ->subscribeNamespace(makePeerSubNs("jp-osa-1"), std::move(nsHandle))
               )
    )
        .start();
  });
  driveUntil([] { return false; }, 5);
  ASSERT_FALSE(result.isReady());

  cancelSource.requestCancellation();
  removeSession(peer);
  peer->fail();
  ASSERT_TRUE(driveUntil([&] { return result.isReady(); }));

  EXPECT_THROW(std::move(result).get(), folly::OperationCancelled);
  EXPECT_TRUE(weakHandle.expired());
  EXPECT_EQ(peer.use_count(), 1);
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

// Records the peering requests a relay sends it. tracksHandle stays weak so a test
// can check the relay released it.
class PeerTracksSession : public NiceMock<MockMoQSession> {
public:
  explicit PeerTracksSession(std::shared_ptr<MoQExecutor> exec)
      : NiceMock<MockMoQSession>(std::move(exec)) {}

  folly::coro::Task<Publisher::SubscribeNamespaceResult>
  subscribeNamespace(SubscribeNamespace subNs, std::shared_ptr<Publisher::NamespacePublishHandle>)
      override {
    ++namespaceRequests;
    co_return std::make_shared<NiceMock<MockSubscribeNamespaceHandle>>(
        SubscribeNamespaceOk{subNs.requestID}
    );
  }

  folly::coro::Task<Publisher::SubscribeTracksResult>
  subscribeTracks(SubscribeTracks subTracks, std::shared_ptr<Publisher::PublishBlockedHandle>)
      override {
    tracksRequests.push_back(subTracks);
    SubscribeTracksOk ok;
    ok.requestID = subTracks.requestID;
    auto handle = std::make_shared<NiceMock<MockSubscribeTracksHandle>>(std::move(ok));
    tracksHandle = handle;
    co_return handle;
  }

  int namespaceRequests{0};
  std::vector<SubscribeTracks> tracksRequests;
  std::weak_ptr<Publisher::SubscribeTracksHandle> tracksHandle;
};

// Draft 18 dropped SUBSCRIBE_NAMESPACE's BOTH option, so a relay also needs
// SUBSCRIBE_TRACKS to receive a peer's pushed tracks.
TEST_P(MoQRelayTest, UpstreamPeeringSubscribesTracksOnDraft18) {
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "sg-sin-2-1");
  for (auto version : {kVersionDraft16, kVersionDraft18}) {
    SCOPED_TRACE(version);
    auto peer = std::make_shared<PeerTracksSession>(exec_);
    ON_CALL(*peer, getNegotiatedVersion()).WillByDefault(Return(std::optional<uint64_t>(version)));
    getOrCreateMockState(peer);

    if (auto* relayExec = relay_->getRelayExec()) {
      folly::coro::co_withExecutor(
          folly::getKeepAliveToken(relayExec),
          relay_->onUpstreamConnect(peer)
      )
          .start();
    } else {
      folly::coro::blockingWait(relay_->onUpstreamConnect(peer), exec_.get());
    }
    const size_t wantTracks = version == kVersionDraft18 ? 1 : 0;
    ASSERT_TRUE(driveUntil([&] {
      return peer->namespaceRequests == 1 && peer->tracksRequests.size() == wantTracks;
    }));
    driveIfMultiThread();
    ASSERT_EQ(peer->tracksRequests.size(), wantTracks);
    if (wantTracks) {
      EXPECT_TRUE(peer->tracksRequests[0].trackNamespacePrefix.empty());
      EXPECT_FALSE(peer->tracksRequests[0].forward);
    }

    verifyOnRelayExec([&] { relay_->onUpstreamDisconnect(peer); });
    driveIfMultiThread();
    EXPECT_TRUE(peer->tracksHandle.expired());
    removeSession(peer);
  }
}

// Each upstream session keeps its own peering handles.
TEST_P(MoQRelayTest, UpstreamDisconnectReleasesOnlyThatSession) {
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "sg-sin-2-1");
  std::vector<std::shared_ptr<PeerTracksSession>> peers;
  for (int i = 0; i < 2; ++i) {
    auto peer = std::make_shared<PeerTracksSession>(exec_);
    ON_CALL(*peer, getNegotiatedVersion())
        .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft18)));
    getOrCreateMockState(peer);
    if (auto* relayExec = relay_->getRelayExec()) {
      folly::coro::co_withExecutor(
          folly::getKeepAliveToken(relayExec),
          relay_->onUpstreamConnect(peer)
      )
          .start();
    } else {
      folly::coro::blockingWait(relay_->onUpstreamConnect(peer), exec_.get());
    }
    ASSERT_TRUE(driveUntil([&] { return peer->tracksRequests.size() == 1; }));
    peers.push_back(std::move(peer));
  }
  driveIfMultiThread();

  verifyOnRelayExec([&] { relay_->onUpstreamDisconnect(peers[0]); });
  driveIfMultiThread();
  EXPECT_TRUE(peers[0]->tracksHandle.expired());
  EXPECT_FALSE(peers[1]->tracksHandle.expired());

  verifyOnRelayExec([&] { relay_->onUpstreamDisconnect(peers[1]); });
  driveIfMultiThread();
  EXPECT_TRUE(peers[1]->tracksHandle.expired());
  for (auto& peer : peers) {
    removeSession(peer);
  }
}

// Holds its SUBSCRIBE_NAMESPACE reply until the test posts the gate.
class GatedPeerSession : public PeerTracksSession {
public:
  using PeerTracksSession::PeerTracksSession;

  folly::coro::Task<Publisher::SubscribeNamespaceResult>
  subscribeNamespace(SubscribeNamespace subNs, std::shared_ptr<Publisher::NamespacePublishHandle>)
      override {
    ++namespaceRequests;
    co_await *gate;
    auto handle = std::make_shared<NiceMock<MockSubscribeNamespaceHandle>>(
        SubscribeNamespaceOk{subNs.requestID}
    );
    namespaceHandle = handle;
    replied = true;
    co_return handle;
  }

  std::shared_ptr<folly::coro::Baton> gate = std::make_shared<folly::coro::Baton>();
  std::weak_ptr<Publisher::SubscribeNamespaceHandle> namespaceHandle;
  bool replied{false};
};

// A disconnect while the peering handshake waits drops that session's handles, and
// the handshake does not go on to SUBSCRIBE_TRACKS.
TEST_P(MoQRelayTest, UpstreamDisconnectDuringHandshakeDropsItsHandles) {
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "sg-sin-2-1");
  auto peer = std::make_shared<GatedPeerSession>(exec_);
  ON_CALL(*peer, getNegotiatedVersion())
      .WillByDefault(Return(std::optional<uint64_t>(kVersionDraft18)));
  getOrCreateMockState(peer);
  if (auto* relayExec = relay_->getRelayExec()) {
    folly::coro::co_withExecutor(
        folly::getKeepAliveToken(relayExec),
        relay_->onUpstreamConnect(peer)
    )
        .start();
  } else {
    folly::coro::co_withExecutor(sessionExec(), relay_->onUpstreamConnect(peer)).start();
  }
  ASSERT_TRUE(driveUntil([&] { return peer->namespaceRequests == 1; }));

  verifyOnRelayExec([&] { relay_->onUpstreamDisconnect(peer); });
  peer->gate->post();
  EXPECT_TRUE(driveUntil([&] { return peer->replied && peer->namespaceHandle.expired(); }));
  driveIfMultiThread();
  EXPECT_TRUE(peer->tracksRequests.empty());
  removeSession(peer);
}

TEST_P(MoQRelayTest, ReciprocalPeeringSubscribesTracksOnDraft18) {
  resetRelay(config::CacheConfig{.maxCachedTracks = 0}, "sg-sin-2-1");
  for (auto version : {kVersionDraft16, kVersionDraft18}) {
    SCOPED_TRACE(version);
    auto peer = std::make_shared<PeerTracksSession>(exec_);
    ON_CALL(*peer, getNegotiatedVersion()).WillByDefault(Return(std::optional<uint64_t>(version)));
    getOrCreateMockState(peer);

    std::shared_ptr<Publisher::SubscribeNamespaceHandle> peerSub;
    auto nsHandle = std::make_shared<NiceMock<MockNamespacePublishHandle>>();
    withSessionContext(peer, [&]() {
      auto result = folly::coro::blockingWait(
          publisherInterface()->subscribeNamespace(makePeerSubNs("jp-osa-1"), nsHandle),
          exec_.get()
      );
      ASSERT_TRUE(result.hasValue());
      peerSub = std::move(result.value());
    });
    driveIfMultiThread();
    EXPECT_EQ(peer->namespaceRequests, 1);
    ASSERT_EQ(peer->tracksRequests.size(), version == kVersionDraft18 ? 1u : 0u);

    peerSub->unsubscribeNamespace();
    driveIfMultiThread();
    EXPECT_TRUE(peer->tracksHandle.expired());
    removeSession(peer);
  }
}

} // namespace openmoq::moqx::test
