/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "peering/MoqtRelayBackend.h"

#include "MoqxRelay.h"
#include "MoqxRelayContext.h"
#include "relay/ExecutorCheck.h"
#include "relay/RelayExecUtil.h"
#include "relay/SessionParticipant.h"

#include <folly/CancellationToken.h>
#include <folly/coro/Collect.h>
#include <folly/coro/Invoke.h>
#include <moxygen/MoQVersions.h>
#include <moxygen/events/MoQFollyExecutorImpl.h>

using namespace moxygen;

namespace openmoq::moqx {

namespace {

constexpr std::chrono::seconds kUpstreamConnectWaitTimeout(5);

std::vector<config::UpstreamConfig> upstreamConfigs(const config::ServiceConfig& svc) {
  if (svc.upstream) {
    return {*svc.upstream};
  }
  return svc.upstreams;
}

} // namespace

MoqtRelayBackend::MoqtRelayBackend(
    std::shared_ptr<MoqxRelay> relay,
    const config::ServiceConfig& service,
    std::optional<uint64_t> hopID
)
    : relay_(relay), relayExec_(relay->getRelayExec()), relayID_(relay->getRelayID()),
      configs_(upstreamConfigs(service)), hopID_(hopID),
      subscriptionBufferBytes_(config::subscriptionBufferBytes(service)) {}

void MoqtRelayBackend::start(folly::EventBase* workerEvb) {
  workerEvb_ = workerEvb;
  auto workerExec = std::make_shared<moxygen::MoQFollyExecutorImpl>(workerEvb);
  for (const auto& cfg : configs_) {
    startUpstream(cfg, workerExec);
  }
}

void MoqtRelayBackend::startUpstream(
    const config::UpstreamConfig& cfg,
    const std::shared_ptr<moxygen::MoQFollyExecutorImpl>& workerExec
) {
  auto verifier = makeUpstreamVerifier(cfg.tls);
  auto relay = relay_.lock();
  XCHECK(relay) << "start() runs while the context holds the relay";
  auto* relayExec = relayExec_;
  // The hooks run on the upstream session's executor. The context keeps this backend until after
  // stop() and the shutdown drain, so neither the hooks nor the work they post outlive it. A
  // suspended handshake depends on its session's close failing it during that drain.
  auto onConnect = [this,
                    relayExec](std::shared_ptr<MoQSession> session) -> folly::coro::Task<void> {
    auto upstream = SessionParticipant::makeParticipant(session, relayExec);
    if (relayExec) {
      co_return co_await folly::coro::co_withExecutor(
          folly::getKeepAliveToken(relayExec),
          onUpstreamConnect(std::move(upstream))
      );
    }
    co_return co_await onUpstreamConnect(std::move(upstream));
  };
  auto onDisconnect = [this, relayExec](std::shared_ptr<MoQSession> session) {
    runOnExec(relayExec, [this, sessionId = session->sessionId()]() {
      onUpstreamDisconnect(sessionId);
    });
  };
  // Mode-aware filters (as inbound sessions): LF mode needs LocalPublishFilter for upstream
  // PUBLISH.
  std::shared_ptr<moxygen::Publisher> pubHandler = relay->createPublisherFilter();
  std::shared_ptr<moxygen::Subscriber> subHandler = relay->createSubscriberFilter();
  auto provider = std::make_shared<UpstreamProvider>(
      workerExec,
      proxygen::URL(cfg.url),
      /*publishHandler=*/pubHandler,
      /*subscribeHandler=*/subHandler,
      relayExec_,
      verifier,
      std::move(onConnect),
      std::move(onDisconnect),
      cfg.connectTimeout,
      cfg.idleTimeout,
      hopID_,
      cfg.relayCost
  );
  provider->setSubscriptionBufferBytes(subscriptionBufferBytes_);
  upstreams_.push_back(provider);

  // Eagerly connect so the peering handshake fires before any subscribers
  // arrive. The connection is lazy in UpstreamProvider but we kick it off
  // now so the upstream namespace tree is ready.
  co_withExecutor(workerExec.get(), provider->start()).start();
}

MoqtRelayBackend::~MoqtRelayBackend() {
  XDCHECK(stopped_ || upstreams_.empty()) << "stop() must run before the backend is destroyed";
}

void MoqtRelayBackend::stop() {
  stopped_ = true;
  for (const auto& upstream : upstreams_) {
    upstream->stop();
    // Do not reset upstreams_ here: the provider's session/client live on the
    // worker EVB thread and must be freed there (via the reconnect coroutine's
    // shared_from_this dropping after it co_returns). The backend's destructor
    // releases upstreams_ after the context drains the executors.
  }
}

folly::coro::Task<void> MoqtRelayBackend::onUpstreamConnect(Participant upstream) {
  XDCHECK(onExecutor(relayExec_));
  connectedUpstreams_.insert(upstream.info->sessionId);
  auto relay = relay_.lock();
  if (!relay) {
    co_return;
  }
  // A disconnect during the handshake erases this entry, so its handles are dropped.
  const auto id = upstream.info->sessionId;
  auto* sessionExec = upstream.info->executor;
  upstreamHandles_.try_emplace(id);
  auto nsHandle = makeNamespaceBridgeHandle(std::move(relay), upstream, {}, relayExec_);
  auto subNs = makePeerSubNs(relayID_);
  auto result = co_await upstream.runOnExecutor(
      [&](Participant::OnExecutor onExecutor
      ) -> folly::coro::Task<Publisher::SubscribeNamespaceResult> {
        co_return co_await onExecutor.publisher().subscribeNamespace(std::move(subNs), nsHandle);
      }
  );
  ExecBound<std::shared_ptr<Publisher::SubscribeNamespaceHandle>> subNsHandle;
  if (result.hasValue()) {
    subNsHandle = {std::move(result.value()), sessionExec};
  } else {
    XLOG(ERR) << "Upstream peer subNs failed: " << result.error().reasonPhrase;
  }
  auto it = upstreamHandles_.find(id);
  if (it == upstreamHandles_.end()) {
    co_return;
  }
  it->second.subNs = std::move(subNsHandle);
  if (getDraftMajorVersion(upstream.info->version) < 18) {
    co_return;
  }
  auto tracks = co_await upstream.runOnExecutor(
      [&](Participant::OnExecutor onExecutor
      ) -> folly::coro::Task<Publisher::SubscribeTracksResult> {
        co_return co_await onExecutor.publisher().subscribeTracks(
            makePeerSubTracks(relayID_),
            nullptr
        );
      }
  );
  ExecBound<std::shared_ptr<Publisher::SubscribeTracksHandle>> tracksHandle;
  if (tracks.hasValue()) {
    tracksHandle = {std::move(tracks.value()), sessionExec};
  } else {
    XLOG(ERR) << "Upstream peer subTracks failed: " << tracks.error().reasonPhrase;
  }
  it = upstreamHandles_.find(id);
  if (it == upstreamHandles_.end()) {
    co_return;
  }
  it->second.tracks = std::move(tracksHandle);
}

void MoqtRelayBackend::onUpstreamDisconnect(SessionId upstream) {
  XDCHECK(onExecutor(relayExec_));
  connectedUpstreams_.erase(upstream);
  upstreamHandles_.erase(upstream);
}

std::optional<std::string> MoqtRelayBackend::authorizePeer(const TrackRequestParameters& params
) const {
  return relayID_.empty() ? std::nullopt : getPeerRelayID(params);
}

folly::coro::Task<void> MoqtRelayBackend::onPeerConnected(Participant peer, std::string peerID) {
  XDCHECK(onExecutor(relayExec_));
  const ParticipantInfo& info = *peer.info;
  peers_[info.sessionId] =
      PeerInfo{.relayID = peerID, .address = info.address, .authority = info.authority};
  auto relay = relay_.lock();
  XCHECK(relay) << "the relay calls its backend";
  // Tag with the peer's relay ID so we suppress echoing these namespaces
  // back to that peer on reconnect.
  auto handle = makeNamespaceBridgeHandle(std::move(relay), peer, peerID, relayExec_);
  auto peerSubNs = makePeerSubNs();
  // The participant's publisher runs the call on the peer session's executor and wraps
  // the returned handle so its teardown hops there too (no token: reciprocal).
  auto recipResult = co_await peer.publisher->subscribeNamespace(std::move(peerSubNs), handle);
  co_await folly::coro::co_safe_point;
  if (recipResult.hasError()) {
    XLOG(ERR) << "Reciprocal peer subNs failed: " << recipResult.error().reasonPhrase;
    co_return;
  }
  std::shared_ptr<Publisher::SubscribeTracksHandle> recipTracks;
  if (getDraftMajorVersion(info.version) >= 18) {
    auto tracks = co_await peer.publisher->subscribeTracks(makePeerSubTracks(relayID_), nullptr);
    co_await folly::coro::co_safe_point;
    if (tracks.hasValue()) {
      recipTracks = std::move(tracks.value());
    } else {
      XLOG(ERR) << "Reciprocal peer subTracks failed: " << tracks.error().reasonPhrase;
    }
  }
  // Gone when the session closed during the reciprocal. Returning drops its handles here.
  auto it = peers_.find(info.sessionId);
  if (it == peers_.end()) {
    co_return;
  }
  it->second.subNs = {std::move(recipResult.value()), info.executor};
  it->second.tracks = {std::move(recipTracks), info.executor};
}

void MoqtRelayBackend::onPeerDisconnected(SessionId session) {
  XDCHECK(onExecutor(relayExec_));
  if (auto it = peers_.find(session); it != peers_.end()) {
    it->second.subNs.reset();
    it->second.tracks.reset();
  }
}

void MoqtRelayBackend::onPeerClosed(SessionId session) {
  XDCHECK(onExecutor(relayExec_));
  peers_.erase(session);
}

std::optional<std::string> MoqtRelayBackend::peerIdOf(SessionId session) const {
  XDCHECK(onExecutor(relayExec_));
  auto it = peers_.find(session);
  return it == peers_.end() ? std::nullopt : std::optional(it->second.relayID);
}

bool MoqtRelayBackend::holdsSessionHandles() const {
  if (!upstreamHandles_.empty()) {
    return true;
  }
  for (const auto& [session, peer] : peers_) {
    if (peer.subNs || peer.tracks) {
      return true;
    }
  }
  return false;
}

folly::coro::Task<void> MoqtRelayBackend::waitForPeers(folly::Function<bool()> routed) {
  folly::CancellationSource routedSource;
  std::vector<folly::coro::Task<void>> waits;
  waits.reserve(upstreams_.size());
  for (const auto& upstream : upstreams_) {
    waits.push_back(folly::coro::co_invoke(
        [&routed, &routedSource, wait = upstream->waitForConnected(kUpstreamConnectWaitTimeout)](
        ) mutable -> folly::coro::Task<void> {
          co_await std::move(wait);
          if (routed()) {
            routedSource.requestCancellation();
          }
        }
    ));
  }
  co_await folly::coro::co_withCancellation(
      folly::cancellation_token_merge(
          co_await folly::coro::co_current_cancellation_token,
          routedSource.getToken()
      ),
      folly::coro::collectAllRange(std::move(waits))
  );
}

folly::coro::Task<void> MoqtRelayBackend::visitPeers(PeerVisitor& visitor) const {
  if (!configs_.empty()) {
    XCHECK(workerEvb_) << "visitPeers() runs after start()";
    // UpstreamProvider lives on the worker EVB and its state_ is a plain enum
    // written there, so read it on that thread rather than the caller's.
    auto states = co_await folly::coro::co_withExecutor(
        static_cast<folly::Executor*>(workerEvb_),
        folly::coro::co_invoke([this]() -> folly::coro::Task<std::vector<UpstreamProvider::State>> {
          std::vector<UpstreamProvider::State> out;
          for (const auto& up : upstreams_) {
            out.push_back(up->state());
          }
          co_return out;
        })
    );
    for (size_t i = 0; i < configs_.size(); ++i) {
      auto state = i < states.size() ? states[i] : UpstreamProvider::State::Disconnected;
      visitor.onPeer(PeerLink{
          .direction = PeerLink::Direction::Dialed,
          .state = state == UpstreamProvider::State::Connected    ? PeerLink::State::Connected
                   : state == UpstreamProvider::State::Connecting ? PeerLink::State::Connecting
                                                                  : PeerLink::State::Disconnected,
          .address = configs_[i].url,
      });
    }
  }
  auto readAccepted = [this]() -> folly::coro::Task<std::vector<PeerLink>> {
    std::vector<PeerLink> out;
    for (const auto& [session, peer] : peers_) {
      out.push_back(PeerLink{
          .direction = PeerLink::Direction::Accepted,
          .state = PeerLink::State::Connected,
          .address = peer.address,
          .authority = peer.authority,
          .relayID = peer.relayID,
      });
    }
    co_return out;
  };
  // peers_ is confined to the relay executor, or to the worker EVB without one.
  folly::Executor* exec = relayExec_ ? relayExec_ : static_cast<folly::Executor*>(workerEvb_);
  std::vector<PeerLink> accepted;
  if (exec) {
    accepted = co_await folly::coro::co_withExecutor(exec, readAccepted());
  } else {
    accepted = co_await readAccepted();
  }
  for (const auto& link : accepted) {
    visitor.onPeer(link);
  }
}

folly::coro::Task<PeerAdminResult> MoqtRelayBackend::executePeerAdminOp(PeerAdminOp op) {
  co_return PeerAdminResult{
      .error = PeerAdminResult::Error::Unsupported,
      .peer = std::move(op.peer),
      .metric = op.metric,
      .message = "MoQT relay peering does not support peer admin"
  };
}

} // namespace openmoq::moqx
