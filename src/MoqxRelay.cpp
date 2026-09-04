/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Originally from github.com/facebookexperimental/moxygen.
 * See the moxygen LICENSE for the original license terms:
 * https://github.com/openmoq/moxygen/blob/main/LICENSE
 *
 * Copyright (c) OpenMOQ contributors.
 */

#include "MoqxRelay.h"
#include "relay/ChannelSubscriber.h"
#include "relay/CrossExecFilter.h"
#include "relay/CrossExecForwarderCallback.h"
#include "relay/InitialTrackState.h"
#include "relay/LocalForwarderCallback.h"
#include "relay/NullConsumers.h"
#include "relay/PublisherCrossExecFilter.h"
#include "relay/SubscriberCrossExecFilter.h"
#include "relay/TrackEventCallback.h"
#include "relay/TrackStatsFilter.h"
#include "relay/WeakRelayForwarderCallback.h"
#include <folly/Random.h>
#include <folly/container/F14Set.h>
#include <folly/coro/Collect.h>
#include <moxygen/MoQFilters.h>
#include <moxygen/MoQTrackProperties.h>

namespace {
constexpr uint8_t kDefaultUpstreamPriority = 128;
constexpr std::chrono::seconds kUpstreamConnectWaitTimeout(5);

// Fire-and-forget an upstream-update coroutine on exec — the
// co_withExecutor(getKeepAliveToken(exec), ...).start() idiom shared by the
// forwarder-callback update paths.
void launchUpdate(folly::Executor* exec, folly::coro::Task<void> task) {
  folly::coro::co_withExecutor(folly::getKeepAliveToken(exec), std::move(task)).start();
}

// Free coroutines, not inline lambdas — captures would dangle past suspension.
folly::coro::Task<void>
doSubscribeUpdate(std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle, bool forward) {
  auto res = co_await handle->requestUpdate(moxygen::RequestUpdate{
      moxygen::RequestID(0),
      handle->subscribeOk().requestID,
      moxygen::kLocationMin,
      moxygen::kLocationMax.group,
      moxygen::kDefaultPriority,
      forward
  });
  if (res.hasError()) {
    XLOG(ERR) << "requestUpdate failed: " << res.error().reasonPhrase;
  }
}

folly::coro::Task<void> doNewGroupRequestUpdate(
    std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle,
    uint64_t group
) {
  XLOG(DBG4) << "Sending NEW_GROUP_REQUEST update: " << group;
  moxygen::RequestUpdate update;
  update.requestID = moxygen::RequestID(0);
  update.existingRequestID = handle->subscribeOk().requestID;
  update.params.insertParam(moxygen::Parameter(
      folly::to_underlying(moxygen::TrackRequestParamKey::NEW_GROUP_REQUEST),
      group
  ));
  auto res = co_await handle->requestUpdate(std::move(update));
  if (res.hasError()) {
    XLOG(ERR) << "NEW_GROUP_REQUEST update failed: " << res.error().reasonPhrase;
  }
}

// Strips the first prefixLen labels off a namespace, yielding the suffix that a
// downstream subscriber (which subscribed with that prefix) should see.
moxygen::TrackNamespace makeNamespaceSuffix(const moxygen::TrackNamespace& src, size_t prefixLen) {
  return moxygen::TrackNamespace(
      std::vector<std::string>(src.trackNamespace.begin() + prefixLen, src.trackNamespace.end())
  );
}

void setOutgoingHopPath(
    moxygen::TrackRequestParameters& params,
    const std::shared_ptr<moxygen::MoQSession>& session,
    const std::vector<uint64_t>& incomingPath,
    uint64_t localHopID
) {
  params.eraseAllParamsOfType(moxygen::TrackRequestParamKey::HOP_PATH);
  if (!session->negotiatedSetupExtension(moxygen::SetupExtension::RelayHops)) {
    return;
  }
  auto version = session->getNegotiatedVersion();
  XCHECK(version.has_value());
  auto outgoingPath = incomingPath;
  outgoingPath.push_back(localHopID);
  auto encodedPath = moxygen::encodeRelayHopPath(outgoingPath, *version);
  XCHECK(encodedPath.hasValue());
  params.insertParam(moxygen::Parameter(
      folly::to_underlying(moxygen::TrackRequestParamKey::HOP_PATH),
      std::move(encodedPath.value())
  ));
}

bool excludesHop(
    const std::optional<uint64_t>& excludedHop,
    const std::vector<uint64_t>& incomingPath,
    uint64_t localHopID
) {
  if (!excludedHop) {
    return false;
  }
  return *excludedHop == localHopID ||
         std::find(incomingPath.begin(), incomingPath.end(), *excludedHop) != incomingPath.end();
}

bool shouldForwardNamespace(
    const std::shared_ptr<moxygen::MoQSession>& publisherSession,
    const std::shared_ptr<moxygen::MoQSession>& subscriberSession,
    moxygen::SubscribeNamespaceOptions options,
    const std::optional<uint64_t>& excludedHop,
    const std::vector<uint64_t>& incomingPath,
    uint64_t localHopID
) {
  return subscriberSession != publisherSession &&
         (options == moxygen::SubscribeNamespaceOptions::NAMESPACE ||
          options == moxygen::SubscribeNamespaceOptions::BOTH) &&
         !excludesHop(excludedHop, incomingPath, localHopID);
}

// Rejects an AbsoluteRange subscription whose endGroup is already behind the
// forwarder's largest group (the client should FETCH instead). Returns
// std::nullopt when the range is acceptable.
std::optional<moxygen::SubscribeError>
checkRangeNotInPast(moxygen::MoQForwarder& fwd, const moxygen::SubscribeRequest& subReq) {
  if (fwd.largest() && subReq.locType == moxygen::LocationType::AbsoluteRange &&
      subReq.endGroup < fwd.largest()->group) {
    return moxygen::SubscribeError{
        subReq.requestID,
        moxygen::SubscribeErrorCode::INVALID_RANGE,
        "Range in the past, use FETCH"
    };
  }
  return std::nullopt;
}

// Derives the upstream SubscribeRequest from a downstream one: fetch from latest at
// upstream priority/default group order, session-assigned requestID, caller's forward.
moxygen::SubscribeRequest makeUpstreamSubReq(moxygen::SubscribeRequest base, bool forward) {
  base.priority = kDefaultUpstreamPriority;
  base.groupOrder = moxygen::GroupOrder::Default;
  base.locType = moxygen::LocationType::LargestObject;
  base.forward = forward;
  base.requestID = moxygen::RequestID(0);
  return base;
}
} // namespace

using namespace moxygen;

namespace openmoq::moqx {

uint64_t generateRelayHopID() {
  uint64_t hopID = 0;
  do {
    folly::Random::secureRandom(&hopID, sizeof(hopID));
    hopID &= kMaxRelayHopID;
  } while (hopID == 0);
  return hopID;
}

// === LocalSubscribeFilter and LocalPublishFilter ===

// LF-mode publish handler: overrides subscribe() to run subscribeFromSubscriberExec
// on the subscriber's executor (no relayExec_ hop). Other Publisher methods fall
// through to PublisherCrossExecFilter.
class MoqxRelay::LocalSubscribeFilter final : public PublisherCrossExecFilter {
public:
  LocalSubscribeFilter(folly::Executor* relayExec, std::shared_ptr<MoqxRelay> relay)
      : PublisherCrossExecFilter(relayExec, relay), relay_(std::move(relay)) {}

  folly::coro::Task<SubscribeResult> subscribe(
      moxygen::SubscribeRequest subReq,
      std::shared_ptr<moxygen::TrackConsumer> consumer
  ) override {
    if (subReq.fullTrackName.trackNamespace.empty()) {
      co_return folly::makeUnexpected(moxygen::SubscribeError{
          subReq.requestID,
          moxygen::SubscribeErrorCode::DOES_NOT_EXIST,
          "namespace required"
      });
    }
    auto session = moxygen::MoQSession::getRequestSession();
    auto* subscriberExec = session->getExecutor();
    // No executor hop: subscribeFromSubscriberExec starts on subscriberExec.
    co_return co_await relay_->subscribeFromSubscriberExec(
        std::move(subReq),
        std::move(consumer),
        std::move(session),
        subscriberExec
    );
  }

  folly::coro::Task<TrackStatusResult> trackStatus(moxygen::TrackStatus req) override {
    // Answer from the local forwarder on this exec; else hop to relayExec_ + upstream.
    if (auto local = relay_->trackStatusOnSubscriberExec(req)) {
      return folly::coro::makeTask<TrackStatusResult>(std::move(*local));
    }
    return PublisherCrossExecFilter::trackStatus(std::move(req));
  }

  folly::coro::Task<FetchResult>
  fetch(moxygen::Fetch fetch, std::shared_ptr<moxygen::FetchConsumer> consumer) override {
    auto session = moxygen::MoQSession::getRequestSession();
    auto resolved = relay_->fetchOnSubscriberExec(std::move(fetch), session);
    // Joining resolved/deferred on subscriberExec; base filter wraps + hops to relayExec_.
    return PublisherCrossExecFilter::fetch(std::move(resolved), std::move(consumer));
  }

private:
  std::shared_ptr<MoqxRelay> relay_;
};

std::shared_ptr<moxygen::Publisher> MoqxRelay::createPublisherFilter() {
  switch (mode()) {
  case Mode::LocalForwarder:
    return std::make_shared<LocalSubscribeFilter>(relayExec_, shared_from_this());
  case Mode::RelayExec:
    return std::make_shared<PublisherCrossExecFilter>(relayExec_, shared_from_this());
  case Mode::SingleThread:
    break;
  }
  return shared_from_this();
}

// LF-mode subscribe handler: overrides publish() to create the publisher's local
// forwarder on its own executor (no cross-exec hop for data). Other Subscriber methods
// fall through to SubscriberCrossExecFilter.
class MoqxRelay::LocalPublishFilter final : public SubscriberCrossExecFilter {
public:
  LocalPublishFilter(folly::Executor* relayExec, std::shared_ptr<MoqxRelay> relay)
      : SubscriberCrossExecFilter(relayExec, relay), relay_(std::move(relay)) {}

  PublishResult publish(
      moxygen::PublishRequest pub,
      std::shared_ptr<moxygen::SubscriptionHandle> handle
  ) override {
    auto session = moxygen::MoQSession::getRequestSession();
    return relay_->publishFromPublisherExec(std::move(pub), std::move(handle), std::move(session));
  }

private:
  std::shared_ptr<MoqxRelay> relay_;
};

std::shared_ptr<moxygen::Subscriber> MoqxRelay::createSubscriberFilter() {
  switch (mode()) {
  case Mode::LocalForwarder:
    return std::make_shared<LocalPublishFilter>(relayExec_, shared_from_this());
  case Mode::RelayExec:
    return std::make_shared<SubscriberCrossExecFilter>(relayExec_, shared_from_this());
  case Mode::SingleThread:
    break;
  }
  return shared_from_this();
}

// Bridges NAMESPACE/NAMESPACE_DONE messages from a peer relay directly into
// MoqxRelay::doPublishNamespace/doPublishNamespaceDone — no coroutine overhead,
// no handle map needed.
class MoqxRelayNamespaceHandle : public Publisher::NamespacePublishHandle {
public:
  MoqxRelayNamespaceHandle(
      std::weak_ptr<MoqxRelay> relay,
      std::shared_ptr<MoQSession> session,
      std::string peerID = {},
      folly::Executor* relayExec = nullptr
  )
      : relay_(std::move(relay)), session_(std::move(session)), peerID_(std::move(peerID)),
        relayExec_(relayExec) {}

  ~MoqxRelayNamespaceHandle() {
    auto relay = relay_.lock();
    if (!relay || activeNamespaces_.empty()) {
      return;
    }
    for (const auto& ns : activeNamespaces_) {
      runOnExec(relayExec_, [relay, ns, session = session_]() mutable {
        relay->doPublishNamespaceDone(ns, session);
      });
    }
  }

  void namespaceMsg(const Namespace& ns) override {
    activeNamespaces_.insert(ns.trackNamespaceSuffix);
    PublishNamespace pubNs;
    pubNs.trackNamespace = ns.trackNamespaceSuffix;
    for (const auto& param : ns.params) {
      pubNs.params.insertParam(param);
    }
    runOnExec(
        relayExec_,
        [relay = relay_, pubNs = std::move(pubNs), session = session_, peerID = peerID_]() mutable {
          if (auto r = relay.lock()) {
            r->doPublishNamespace(std::move(pubNs), session, nullptr, peerID);
          }
        }
    );
  }

  void namespaceMsg(const TrackNamespace& suffix) override {
    Namespace ns;
    ns.trackNamespaceSuffix = suffix;
    namespaceMsg(ns);
  }

  void namespaceDoneMsg(const TrackNamespace& suffix) override {
    activeNamespaces_.erase(suffix);
    runOnExec(relayExec_, [relay = relay_, suffix, session = session_]() mutable {
      if (auto r = relay.lock()) {
        r->doPublishNamespaceDone(suffix, session);
      }
    });
  }

private:
  std::weak_ptr<MoqxRelay> relay_;
  std::shared_ptr<MoQSession> session_;
  std::string peerID_;
  folly::Executor* relayExec_;
  folly::F14FastSet<TrackNamespace, TrackNamespace::hash> activeNamespaces_;
};

std::shared_ptr<Publisher::NamespacePublishHandle> makeNamespaceBridgeHandle(
    std::weak_ptr<MoqxRelay> relay,
    std::shared_ptr<MoQSession> session,
    std::string peerID,
    folly::Executor* relayExec
) {
  return std::make_shared<MoqxRelayNamespaceHandle>(
      std::move(relay),
      std::move(session),
      std::move(peerID),
      relayExec
  );
}

folly::coro::Task<void> MoqxRelay::onUpstreamConnect(std::shared_ptr<MoQSession> session) {
  co_return co_await onUpstreamConnectImpl(std::move(session));
}

folly::coro::Task<void> MoqxRelay::onUpstreamConnectImpl(std::shared_ptr<MoQSession> session) {
  auto nsHandle = makeNamespaceBridgeHandle(weak_from_this(), session, {}, relayExec_);
  auto subNs = makePeerSubNs(relayID_);
  if (session->negotiatedSetupExtension(SetupExtension::RelayHops)) {
    subNs.params.insertParam(
        Parameter(folly::to_underlying(TrackRequestParamKey::EXCLUDE_HOP), relayHopID_)
    );
  }
  // subscribeNamespace must run on the upstream session's executor
  auto result = co_await folly::coro::co_withExecutor(
      folly::getKeepAliveToken(session->getExecutor()),
      session->subscribeNamespace(std::move(subNs), nsHandle)
  );
  if (result.hasValue()) {
    upstreamSubNsHandle_ = std::move(result.value());
  } else {
    XLOG(ERR) << "MoqxRelay: upstream peer subNs failed: " << result.error().reasonPhrase;
  }
}

void MoqxRelay::onSessionEnd(std::shared_ptr<MoQSession> session) {
  // Raw key plus an owner compare: neither takes a strong ref, so the session is never
  // released on relayExec_. lock() here would reintroduce that bug.
  runOnExec(
      relayExec_,
      [self = weak_from_this(), key = session.get(), weak = std::weak_ptr<MoQSession>(session)]() {
        auto relay = self.lock();
        if (!relay) {
          return;
        }
        auto it = relay->legacyPublisherHopIDs_.find(key);
        if (it != relay->legacyPublisherHopIDs_.end() && !it->second.session.owner_before(weak) &&
            !weak.owner_before(it->second.session)) {
          relay->legacyPublisherHopIDs_.erase(it);
        }
      }
  );
}

void MoqxRelay::onUpstreamDisconnect() {
  upstreamSubNsHandle_.reset();
}

std::shared_ptr<Subscriber::PublishNamespaceHandle> MoqxRelay::doPublishNamespace(
    PublishNamespace pubNs,
    std::shared_ptr<MoQSession> session,
    std::shared_ptr<Subscriber::PublishNamespaceCallback> callback,
    std::string peerID
) {
  XLOG(DBG1) << __func__ << " ns=" << pubNs.trackNamespace;
  auto relayHopPath = ingestRelayHopPath(pubNs, session);
  if (!relayHopPath) {
    return nullptr;
  }
  if (!pubNs.trackNamespace.startsWith(allowedNamespacePrefix_)) {
    return nullptr;
  }
  auto [nodePtr, sessions, replacedSession] = namespaceTree_.setPublisher(
      pubNs.trackNamespace,
      session,
      std::move(callback),
      std::move(peerID),
      pubNs.requestID,
      *relayHopPath
  );
  if (replacedSession) {
    XLOG(WARNING) << "PublishNamespace: Existing session (" << replacedSession.get()
                  << ") has already published trackNamespace=" << pubNs.trackNamespace;
    // Remove ongoing subscriptions for the replaced publisher.
    registry_.removeIf([&](const SubscriptionRegistry::EntryView& e) {
      if (e.ftn.trackNamespace.startsWith(pubNs.trackNamespace) && e.upstream == replacedSession) {
        XLOG(DBG4) << "Erasing subscription to " << e.ftn;
        return true;
      }
      return false;
    });
  }
  for (auto& [outSession, info] : sessions) {
    if (shouldForwardNamespace(
            session,
            outSession,
            info.options,
            info.excludeHop,
            *relayHopPath,
            relayHopID_
        )) {
      // Bidi NAMESPACE is draft 16+ only; the handle is populated regardless of
      // version, so gate on it (matching doPublishNamespaceDone).
      auto maybeVersion = outSession->getNegotiatedVersion();
      if (maybeVersion.has_value() && getDraftMajorVersion(*maybeVersion) >= 16 &&
          info.namespacePublishHandle) {
        auto suffix = makeNamespaceSuffix(pubNs.trackNamespace, info.trackNamespacePrefix.size());
        Namespace ns;
        ns.trackNamespaceSuffix = std::move(suffix);
        setOutgoingHopPath(ns.params, outSession, *relayHopPath, relayHopID_);
        info.namespacePublishHandle->namespaceMsg(ns);
      } else {
        // Draft <= 15: send PUBLISH_NAMESPACE on a new stream
        auto outgoingPubNs = pubNs;
        setOutgoingHopPath(outgoingPubNs.params, outSession, *relayHopPath, relayHopID_);
        auto exec = outSession->getExecutor();
        co_withExecutor(
            exec,
            publishNamespaceToSession(outSession, std::move(outgoingPubNs), nodePtr)
        )
            .start();
      }
    }
  }
  return nodePtr;
}

std::optional<std::vector<uint64_t>> MoqxRelay::ingestRelayHopPath(
    const PublishNamespace& pubNs,
    const std::shared_ptr<MoQSession>& session
) {
  std::vector<uint64_t> relayHopPath;
  if (!session->negotiatedSetupExtension(SetupExtension::RelayHops)) {
    relayHopPath.push_back(getOrCreateLegacyPublisherHopID(session));
  } else {
    const auto* hopPathParam = pubNs.params.getFirstParam(TrackRequestParamKey::HOP_PATH);
    if (!hopPathParam) {
      XLOG(WARN) << "Dropping namespace without required HOP_PATH ns=" << pubNs.trackNamespace;
      return std::nullopt;
    }
    auto version = session->getNegotiatedVersion();
    XCHECK(version.has_value());
    auto decoded = decodeRelayHopPath(hopPathParam->asString, *version);
    if (decoded.hasError()) {
      XLOG(WARN) << "Closing session for malformed HOP_PATH ns=" << pubNs.trackNamespace;
      session->close(SessionCloseErrorCode::PROTOCOL_VIOLATION);
      return std::nullopt;
    }
    relayHopPath = std::move(decoded.value());
  }

  if (std::find(relayHopPath.begin(), relayHopPath.end(), relayHopID_) != relayHopPath.end()) {
    XLOG(DBG1) << "Dropping looped namespace ns=" << pubNs.trackNamespace;
    return std::nullopt;
  }
  return relayHopPath;
}

uint64_t MoqxRelay::getOrCreateLegacyPublisherHopID(const std::shared_ptr<MoQSession>& session) {
  const auto* key = session.get();
  auto it = legacyPublisherHopIDs_.find(key);
  if (it != legacyPublisherHopIDs_.end()) {
    auto existing = it->second.session.lock();
    if (existing == session) {
      return it->second.hopID;
    }
    // Stale only if onSessionEnd was missed and the address was recycled.
    legacyPublisherHopIDs_.erase(it);
  }

  auto hopID = generateRelayHopID();
  legacyPublisherHopIDs_.emplace(key, LegacyPublisherHopID{session, hopID});
  return hopID;
}

folly::coro::Task<Subscriber::PublishNamespaceResult> MoqxRelay::publishNamespace(
    PublishNamespace pubNs,
    std::shared_ptr<Subscriber::PublishNamespaceCallback> callback
) {
  return publishNamespaceImpl(std::move(pubNs), std::move(callback));
}

folly::coro::Task<Subscriber::PublishNamespaceResult> MoqxRelay::publishNamespaceImpl(
    PublishNamespace pubNs,
    std::shared_ptr<Subscriber::PublishNamespaceCallback> callback
) {
  // TODO: store auth for forwarding on future SubscribeNamespace?
  auto session = MoQSession::getRequestSession();
  auto requestID = pubNs.requestID;
  auto result = doPublishNamespace(std::move(pubNs), session, std::move(callback));
  if (!result) {
    co_return folly::makeUnexpected(
        PublishNamespaceError{requestID, PublishNamespaceErrorCode::UNINTERESTED, "bad namespace"}
    );
  }
  co_return result;
}

folly::coro::Task<void> MoqxRelay::publishNamespaceToSession(
    std::shared_ptr<MoQSession> session,
    PublishNamespace pubNs,
    std::shared_ptr<NamespaceTree::NamespaceNode> nodePtr
) {
  auto publishNamespaceHandle = co_await session->publishNamespace(pubNs);
  if (publishNamespaceHandle.hasError()) {
    XLOG(ERR) << "PublishNamespace failed err=" << publishNamespaceHandle.error().reasonPhrase;
  } else {
    // This can race with unsubscribeNamespace
    nodePtr->addDraft14PublishNamespaceHandle(session, std::move(publishNamespaceHandle.value()));
  }
}

void MoqxRelay::doPublishNamespaceDone(
    const TrackNamespace& trackNamespace,
    std::shared_ptr<MoQSession> session
) {
  XLOG(DBG1) << __func__ << " ns=" << trackNamespace;
  auto result = namespaceTree_.unpublishNamespace(trackNamespace, session);
  if (result.hasError()) {
    if (result.error() == NamespaceTree::Error::NodeNotFound) {
      XLOG(DBG1) << "Node already pruned for ns=" << trackNamespace;
    } else {
      XLOG(DBG1) << "Ignoring publishNamespaceDone for ns=" << trackNamespace
                 << " (no owner or non-owner session)";
    }
    return;
  }
  // Draft <= 15: dispatch publishNamespaceDone on each subscriber's executor
  for (auto& [sess, handle] : result.value().legacyHandles) {
    sess->getExecutor()->add([h = handle] { h->publishNamespaceDone(); });
  }
  // Draft >= 16: send NAMESPACE_DONE on the bidi stream
  for (auto& [outSession, info] : result.value().subscribers) {
    // Same predicate as the advertisement, so a subscriber excluded then is not
    // told a namespace it never heard about is done.
    if (shouldForwardNamespace(
            session,
            outSession,
            info.options,
            info.excludeHop,
            result.value().relayHopPath,
            relayHopID_
        )) {
      auto maybeVersion = outSession->getNegotiatedVersion();
      if (maybeVersion.has_value() && getDraftMajorVersion(*maybeVersion) >= 16) {
        if (info.namespacePublishHandle) {
          auto suffix = makeNamespaceSuffix(trackNamespace, info.trackNamespacePrefix.size());
          info.namespacePublishHandle->namespaceDoneMsg(suffix);
        }
      }
    }
  }
}

void MoqxRelay::onPublishNamespaceDone(const TrackNamespace& trackNamespace) {
  doPublishNamespaceDone(trackNamespace, MoQSession::getRequestSession());
}

void MoqxRelay::onPublishDone(const FullTrackName& ftn) {
  XLOG(DBG1) << __func__ << " ftn=" << ftn;

  auto upstreamView = registry_.getUpstreamView(ftn);
  if (upstreamView && upstreamView->isPublish) {
    namespaceTree_.unpublishTrack(ftn.trackNamespace, ftn.trackName);
  }

  if (mode() == Mode::LocalForwarder) {
    // The publisher forwarder is [Pub]-owned — never inspect it from relayExec_.
    // The entry only indexes a live publisher, so drop it outright.
    registry_.remove(ftn);
  } else {
    // Clears handle + upstream; erases if no subscribers remain.
    auto kept = registry_.onPublisherTerminated(ftn);
    if (!kept) {
      XLOG(DBG1) << "Publisher terminated with no subscribers, cleaning up " << ftn;
    }
  }
}

// Validates a publish namespace against allowedNamespacePrefix_ (UNINTERESTED)
// and non-emptiness (INTERNAL_ERROR). Returns std::nullopt on success. Safe to
// call on any thread (reads only immutable config).
std::optional<PublishError>
MoqxRelay::validatePublishNamespace(const FullTrackName& ftn, RequestID requestID) const {
  if (!ftn.trackNamespace.startsWith(allowedNamespacePrefix_)) {
    return PublishError{requestID, PublishErrorCode::UNINTERESTED, "bad namespace"};
  }
  if (ftn.trackNamespace.empty()) {
    return PublishError{requestID, PublishErrorCode::INTERNAL_ERROR, "namespace required"};
  }
  return std::nullopt;
}

// Chain and entry claim are one call: LocalForwarderCallback vacates the entry when the
// source ends, so a chain over a forwarder that does not hold it has nothing to remove.
LocalForwarderRegistry::ParkResult MoqxRelay::installPublisherForwarder(
    const FullTrackName& ftn,
    const std::shared_ptr<MoQForwarder>& fwd,
    InstallKind kind
) {
  auto& localReg = localRegistry();
  auto relayAdapter = std::make_shared<WeakRelayForwarderCallback>(weak_from_this());
  auto crossExec =
      std::make_shared<CrossExecForwarderCallback>(relayExec_, std::move(relayAdapter));
  bool removeOnEmpty = kind == InstallKind::FromSubscribe;
  fwd->setCallback(
      std::make_shared<LocalForwarderCallback>(&localReg, ftn, std::move(crossExec), removeOnEmpty)
  );

  if (kind == InstallKind::FromPublish) {
    return localReg.replaceAndPark(ftn, fwd);
  }
  return {localReg.replace(ftn, fwd), LocalForwarderRegistry::ParkTicket{}};
}

// Called from LocalPublishFilter::publish() on publisherExec. Creates the publisher's
// local forwarder and sets up the publisher forwarder on relayExec_.
Subscriber::PublishResult MoqxRelay::publishFromPublisherExec(
    PublishRequest pub,
    std::shared_ptr<Publisher::SubscriptionHandle> handle,
    std::shared_ptr<MoQSession> session
) {
  if (auto err = validatePublishNamespace(pub.fullTrackName, pub.requestID)) {
    return folly::makeUnexpected(std::move(*err));
  }

  auto localPubFwd = std::make_shared<MoQForwarder>(pub.fullTrackName);
  // Install the new forwarder and return the identity of the one that was displaced, if any.
  // Either a publisher or a subscriber forwarder could be displaced. Either way, the relay exec
  // hop below initiates publishDone on the publisher forwarder's exec in publishWithSession.
  auto install =
      installPublisherForwarder(pub.fullTrackName, localPubFwd, InstallKind::FromPublish);
  install.claim.markReady(InitialTrackState{pub.largest, pub.extensions});

  // crossExecFilter is a channel subscriber for the relay exec
  // registerPublishOnRelayExec completes wiring the chain (topNFilter → terminationFilter →
  // cache).
  auto crossExecFilter = CrossExecFilter::create(relayExec_, nullptr);
  // forward=true + passive=true: internal relay chain observes all objects but
  // does not count as a real forwarding subscriber.
  localPubFwd
      ->addChannelSubscriber(relayExec_, /*forward=*/true, crossExecFilter, /*passive=*/true);

  auto publisherRef = makeForwarderRef(localPubFwd, session->getExecutor());
  auto ftn = pub.fullTrackName;
  auto reply = folly::coro::co_invoke(
      [exec = relayExec_,
       relay = shared_from_this(),
       ftn = std::move(ftn),
       displacedTicket = std::move(install.displaced),
       pub = std::move(pub),
       handle = std::move(handle),
       session = std::move(session),
       publisherRef = std::move(publisherRef),
       crossExecFilter]() mutable -> folly::coro::Task<folly::Expected<PublishOk, PublishError>> {
        auto result = co_await folly::coro::co_awaitTry(folly::coro::co_withExecutor(
            folly::getKeepAliveToken(exec),
            relay->registerPublishOnRelayExec(
                std::move(pub),
                std::move(handle),
                std::move(session),
                std::move(publisherRef),
                std::move(crossExecFilter)
            )
        ));
        // Now that the new forwarder has been registered on relayExec_, it's safe to discard the
        // last strong reference to a displaced entry.
        (void)relay->localRegistry().takeDisplaced(ftn, std::move(displacedTicket));

        // This can throw if there was an error.
        co_return std::move(result).value();
      }
  );

  auto consumer = std::static_pointer_cast<TrackConsumer>(std::move(localPubFwd));
  return PublishConsumerAndReplyTask{std::move(consumer), std::move(reply)};
}

// Runs on relayExec_. Registers publisherRef in the registry and wires
// relayChainFilter to the topN filter.
folly::coro::Task<folly::Expected<PublishOk, PublishError>> MoqxRelay::registerPublishOnRelayExec(
    PublishRequest pub,
    std::shared_ptr<Publisher::SubscriptionHandle> handle,
    std::shared_ptr<MoQSession> session,
    ForwarderRef publisherRef,
    std::shared_ptr<CrossExecFilter> relayChainFilter
) {
  auto ftn = pub.fullTrackName;
  auto setup = publishWithSession(
      std::move(pub),
      std::move(handle),
      std::move(session),
      std::move(publisherRef)
  );
  if (setup.hasError()) {
    co_return folly::makeUnexpected(setup.error());
  }

  auto topNView = registry_.getTopNView(ftn);
  XCHECK(topNView && topNView->chainHead)
      << "registerPublishOnRelayExec: relay chain always present in MT mode";
  relayChainFilter->setDownstream(topNView->chainHead);

  co_return setup.value().publishOk;
}

// Publisher::publish entry point for SingleThread/RelayExec modes (LF mode uses
// publishFromPublisherExec instead).
Subscriber::PublishResult
MoqxRelay::publish(PublishRequest pub, std::shared_ptr<Publisher::SubscriptionHandle> handle) {
  XLOG(DBG1) << __func__ << " ftn=" << pub.fullTrackName;
  XCHECK(handle) << "Publish handle cannot be null";
  // getRequestSession() stays valid on relayExec_: RequestContext propagates
  // across the filter's executor hop. Validate before touching state.
  auto session = MoQSession::getRequestSession();
  if (auto err = validatePublishNamespace(pub.fullTrackName, pub.requestID)) {
    return folly::makeUnexpected(std::move(*err));
  }
  XCHECK(mode() != Mode::LocalForwarder) << "publish() bypassed by LocalPublishFilter in LF mode";

  maybeSetSessionExec(*session);
  auto forwarder = std::make_shared<MoQForwarder>(pub.fullTrackName, pub.largest);
  forwarder->setExtensions(pub.extensions);
  auto publisherRef = makeForwarderRef(forwarder, session->getExecutor());
  auto setup = publishWithSession(
      std::move(pub),
      std::move(handle),
      std::move(session),
      std::move(publisherRef)
  );
  if (setup.hasError()) {
    return folly::makeUnexpected(setup.error());
  }
  return PublishConsumerAndReplyTask{
      std::move(setup.value().consumer),
      folly::coro::makeTask<folly::Expected<PublishOk, PublishError>>(
          folly::Expected<PublishOk, PublishError>(std::move(setup.value().publishOk))
      )
  };
}

MoqxRelay::PublishSetupResult MoqxRelay::publishWithSession(
    PublishRequest pub,
    std::shared_ptr<Publisher::SubscriptionHandle> handle,
    std::shared_ptr<MoQSession> session,
    ForwarderRef publisherRef
) {
  std::shared_ptr<MoQForwarder> chainForwarder;
  if (mode() != Mode::LocalForwarder) {
    // Strong ref to forwarder is allowed in non-LF modes
    chainForwarder = publisherRef.getIfOwned();
  }

  // Handle duplicate publisher at relay level before registering in the tree.
  auto publisherWrapped = maybeWrapPublisher(relayExec_, session);
  auto publishEntry = registry_.createFromPublish(
      pub.fullTrackName,
      publisherRef,
      session,
      std::move(publisherWrapped),
      pub.requestID,
      std::move(handle),
      [&] { return buildFilterChain(pub.fullTrackName, chainForwarder); }
  );

  if (publishEntry.evicted) {
    XLOG(DBG1) << "New publisher for existing subscription";
    auto& evicted = *publishEntry.evicted;
    // Null handle => previous publisher already terminated and onPublishDone() tore it down; skip.
    if (evicted.handle) {
      // Depending on mode, this is inline or a hop to publisherExec.
      runOnSessionExec(relayExec_, evicted.publisherExec, [h = evicted.handle] {
        h->unsubscribe();
      });
      PublishDone done{
          RequestID(0),
          PublishDoneStatusCode::SUBSCRIPTION_ENDED,
          0, // filled in by session
          "upstream disconnect"
      };
      evicted.forwarder.post([done = std::move(done)](MoQForwarder& f) mutable {
        f.publishDone(std::move(done));
      });
    }
  }

  auto topNFilter = registry_.getTopNView(pub.fullTrackName)->topNFilter;

  // Register in the namespace tree. The ranking callback fires once per
  // PropertyRanking on the path from this node to the root — registering the
  // track and wiring observers so TRACK_FILTER subscribers see it.
  auto [nodePtr, sessions] = namespaceTree_.addPublish(
      pub.fullTrackName,
      session,
      [&](uint64_t propertyType, const std::shared_ptr<PropertyRanking>& ranking) {
        auto initialPropertyValue = pub.extensions.getIntExtension(propertyType);
        ranking->registerTrack(pub.fullTrackName, initialPropertyValue, session);
        topNFilter->registerObserver(
            propertyType,
            PropertyObserver{
                .onValueChanged = [ranking, ftn = pub.fullTrackName](uint64_t value
                                  ) { ranking->updateSortValue(ftn, value); },
                .onTrackEnded = [ranking, ftn = pub.fullTrackName]() { ranking->removeTrack(ftn); },
                .onActivity = [ranking]() { ranking->sweepIdle(); }
            }
        );
      }
  );

  switch (mode()) {
  case Mode::SingleThread:
  case Mode::RelayExec:
    // Weak ref breaks the registry → forwarder → callback → relay cycle.
    XCHECK(chainForwarder) << "publishWithSession: null chainForwarder in non-LF mode";
    chainForwarder->setCallback(std::make_shared<WeakRelayForwarderCallback>(weak_from_this()));
    break;
  case Mode::LocalForwarder:
    // Local forwarder already had its CrossExecForwarderCallback installed by
    // publishFromPublisherExec (dispatches onEmpty to relayExec_); don't overwrite.
    break;
  }

  uint64_t nSubscribers = 0;
  bool hasTrackFilterSub = false;
  for (auto& [outSession, info] : sessions) {
    if (info.trackFilter) {
      // TRACK_FILTER subscribers: PropertyRanking handles selection via
      // onTrackSelected; don't publish directly here.
      hasTrackFilterSub = true;
      continue;
    }
    if (outSession != session && (info.options == SubscribeNamespaceOptions::PUBLISH ||
                                  info.options == SubscribeNamespaceOptions::BOTH)) {
      nSubscribers++;
      if (!addSubscriberAndPublish(outSession, publisherRef, info.forward, /*pinned=*/true)) {
        XLOG(ERR) << "addSubscriberAndPublish failed for " << pub.fullTrackName;
        continue;
      }
    }
  }

  // Draft 18+: also fan out to SUBSCRIBE_TRACKS subscribers from the parallel
  // tracks tree. They live in an independent overlap space and only want
  // PUBLISH messages (no NAMESPACE / NAMESPACE_DONE).
  NamespaceTree::SessionSubscriberList tracksSessions;
  auto tracksNode = tracksTree_.findNode(
      pub.fullTrackName.trackNamespace,
      /*createMissingNodes=*/false,
      NamespaceTree::MatchType::Exact,
      &tracksSessions
  );
  if (tracksNode) {
    tracksNode->forEachSubscriber(
        [&](const std::shared_ptr<MoQSession>& outSession,
            const NamespaceTree::NamespaceNode::NamespaceSubscriberInfo& info) {
          tracksSessions.emplace_back(outSession, info);
        }
    );
  }
  for (auto& [outSession, info] : tracksSessions) {
    if (outSession != session) {
      nSubscribers++;
      if (!addSubscriberAndPublish(outSession, publisherRef, info.forward, /*pinned=*/true)) {
        XLOG(ERR) << "addSubscriberAndPublish failed for " << pub.fullTrackName;
        continue;
      }
    }
  }

  // Forward if there are direct subscribers OR TRACK_FILTER subscribers
  // (PropertyRanking needs objects to evaluate property values for ranking).
  // When subscribers join later via subscribeNamespace, forwardChanged() sends REQUEST_UPDATE.
  bool shouldForward = (nSubscribers > 0) || hasTrackFilterSub;

  return PublishSetup{
      publishEntry.consumer,
      PublishOk{
          pub.requestID,
          /*forward=*/shouldForward,
          kDefaultPriority,
          pub.groupOrder,
          LocationType::AbsoluteRange,
          kLocationMin,
          kLocationMax.group
      }
  };
}

namespace {

folly::coro::Task<void> awaitPublishReply(
    std::shared_ptr<MoQForwarder> forwarder, // keeps subscriber's raw ref alive
    std::shared_ptr<MoQForwarder::Subscriber> subscriber,
    folly::coro::Task<folly::Expected<PublishOk, PublishError>> reply
) {
  auto result = co_await co_awaitTry(std::move(reply));
  if (result.hasException()) {
    XLOG(ERR) << "Publish reply exception for " << forwarder->fullTrackName()
              << " subscriber=" << subscriber.get() << ": " << result.exception().what();
    subscriber->unsubscribe();
    co_return;
  }
  if (result.value().hasError()) {
    XLOG(ERR) << "Publish reply error for " << forwarder->fullTrackName()
              << " subscriber=" << subscriber.get() << ": " << result.value().error().reasonPhrase;
    subscriber->unsubscribe();
    co_return;
  }
  XLOG(DBG1) << "Received PublishOk for " << forwarder->fullTrackName()
             << " subscriber=" << subscriber.get();
  subscriber->onPublishOk(result.value().value());
}

} // namespace

// Sync setup: addSubscriber → set pinned → session->publish (optionally via
// SubscriberCrossExecFilter when subscriberExec is non-null) → set trackConsumer.
// Returns nullopt and cleans up on any synchronous failure.
std::optional<MoqxRelay::PreparedPublish> MoqxRelay::startPublish(
    std::shared_ptr<MoQSession> session,
    std::shared_ptr<MoQForwarder> forwarder,
    bool forward,
    bool pinned,
    folly::Executor* subscriberExec
) {
  auto subscriber = forwarder->addSubscriber(session, forward);
  if (!subscriber) {
    XLOG(ERR) << "startPublish: addSubscriber null for " << forwarder->fullTrackName();
    return std::nullopt;
  }
  subscriber->pinned = pinned;
  Subscriber::PublishResult pub;
  if (subscriberExec) {
    SubscriberCrossExecFilter wrapped(subscriberExec, session);
    pub = wrapped.publish(subscriber->getPublishRequest(), subscriber);
  } else {
    pub = session->publish(subscriber->getPublishRequest(), subscriber);
  }
  if (pub.hasError()) {
    XLOG(ERR) << "startPublish: publish failed: " << pub.error().reasonPhrase;
    subscriber->unsubscribe();
    return std::nullopt;
  }
  subscriber->trackConsumer = wrapWithTrackStats(
      trackStats_,
      forwarder->fullTrackName(),
      std::move(pub->consumer),
      stats::TrackDirection::Egress
  );
  return PreparedPublish{std::move(subscriber), std::move(pub->reply)};
}

// In relay+local-forwarder mode: dispatches addSubscriberAndPublishViaLocalForwarder
// to subscriberExec. Otherwise: calls startPublish sync and fires reply async.
// Returns false on synchronous failure.
bool MoqxRelay::addSubscriberAndPublish(
    std::shared_ptr<MoQSession> subscriberSession,
    const ForwarderRef& publisherRef,
    bool forward,
    bool pinned
) {
  XCHECK(publisherRef) << "addSubscriberAndPublish: empty forwarder ref";
  if (mode() == Mode::LocalForwarder) {
    // TODO: we don't want to complete the publisher's replyTask until we've initiated
    // publish and attached the consumer for every SUB_NS subscriber.  So .start()
    // is not correct in LF mode.
    co_withExecutor(
        folly::getKeepAliveToken(subscriberSession->getExecutor()),
        addSubscriberAndPublishViaLocalForwarder(
            subscriberSession,
            publisherRef.track(),
            forward,
            pinned
        )
    )
        .start();
    return true;
  }
  auto forwarder = publisherRef.getIfOwned();
  XCHECK(forwarder) << "addSubscriberAndPublish: remote ref outside LocalForwarder mode";
  folly::Executor* subscriberExec = relayExec_ ? subscriberSession->getExecutor() : nullptr;
  auto p = startPublish(subscriberSession, forwarder, forward, pinned, subscriberExec);
  if (!p) {
    return false;
  }
  // On relayExec() so onPublishOk and detach() (from publishDone) cannot race.
  auto exec = relayExec();
  co_withExecutor(
      folly::getKeepAliveToken(exec),
      awaitPublishReply(forwarder, std::move(p->subscriber), std::move(p->reply))
  )
      .start();
  return true;
}

namespace {

// Tears down an aborted local-forwarder setup: drops the channel sub(s) on the publisher
// and drains subscriberLocalFwd via publishDone if given.  Only the first subscriber that
// owns the relay chain can remove the relayExec's channel sub.
// The drain runs inline, so a caller supplying subscriberLocalFwd must be on subscriberExec.
void teardownLocalForwarderOnFailure(
    const openmoq::moqx::ForwarderRef& publisherRef,
    folly::Executor* subscriberExec,
    folly::Executor* relayExec,
    const std::shared_ptr<MoQForwarder>& subscriberLocalFwd = nullptr,
    std::string publishDoneReason = {}
) {
  if (publisherRef) {
    publisherRef.post([ex = subscriberExec, re = relayExec](MoQForwarder& pf) {
      pf.removeChannelSubscriberByExec(ex);
      if (re) {
        pf.removeChannelSubscriberByExec(re);
      }
    });
  }
  if (subscriberLocalFwd) {
    subscriberLocalFwd->publishDone(PublishDone{
        RequestID(0),
        PublishDoneStatusCode::INTERNAL_ERROR,
        0,
        std::move(publishDoneReason)
    });
  }
}

// === MoQForwarder::Callback chain overview ===
//
// MoQForwarder fires three callbacks: onEmpty (last subscriber left),
// forwardChanged (forwarding subscriber count crossed zero), and
// newGroupRequested (subscriber issued a NEW_GROUP_REQUEST).
//
// Single-threaded mode:
//   forwarder.callback = MoqxRelay (direct, no hop)
//
// Multi-threaded — publisher forwarder (lives on publisherExec):
//   publisherFwd.callback =
//     LocalForwarderCallback(localReg, ftn,
//       CrossExecForwarderCallback(relayExec_,
//         WeakRelayForwarderCallback(relay)))
//
//   [publisherExec] LocalForwarderCallback: removes from localReg on onEmpty
//                 (removeOnEmpty=false when publish-initiated, true when subscribe-initiated)
//       ↓ (CrossExecForwarderCallback dispatches to relayExec_ fire-and-forget)
//   [relayExec_]  WeakRelayForwarderCallback: recovers relay via weak_ptr,
//                 calls onEmptyImpl / forwardChangedImpl / newGroupRequestedImpl
//
// Multi-threaded — local forwarder (lives on subscriberExec, subscribe path):
//   During setup window:
//     localFwd.callback = PendingForwarderCallback
//       captures events; replayed onto finalCallback after setup
//
//   After setup:
//     localFwd.callback =
//       LocalForwarderCallback(localReg, ftn,
//         CrossExecForwarderCallback(publisherExec,
//           ChannelForwarderCallback(channelSub, subscriberExec)))
//
//   [subscriberExec] LocalForwarderCallback: removes from localReg on onEmpty,
//                    passes through forwardChanged / newGroupRequested
//       ↓ (CrossExecForwarderCallback dispatches to publisherExec)
//   [publisherExec]    ChannelForwarderCallback:
//                      onEmpty → publisherFwd->removeChannelSubscriberByExec(subscriberExec)
//                                (may cascade into publisherFwd's own callback chain above)
//                      forwardChanged / newGroupRequested → launch background coro
//                                                           → requestUpdate(handle_)
//
// Weak-ptr discipline:
//   WeakRelayForwarderCallback holds weak_ptr<relay> to break the cycle
//   registry → forwarder → callback → relay → registry.
//   CrossExecForwarderCallback reads the track name from the forwarder that invoked it
//   — on the owner thread, where the forwarder is alive.

// Captures forwardChanged/newGroupRequested/onEmpty during setup (join→setCallback window)
// so they can be replayed once the real callback is installed.
class PendingForwarderCallback : public moxygen::MoQForwarder::Callback {
public:
  PendingForwarderCallback(
      openmoq::moqx::LocalForwarderRegistry* localReg,
      moxygen::FullTrackName ftn
  )
      : localReg_(localReg), ftn_(std::move(ftn)) {}

  void forwardChanged(moxygen::MoQForwarder*, bool f) override { lastForward_ = f; }
  void newGroupRequested(moxygen::MoQForwarder*, uint64_t g) override {
    maxGroup_ = std::max(maxGroup_.value_or(0), g);
  }
  void onEmpty(moxygen::MoQForwarder* forwarder) override {
    localReg_->remove(ftn_, forwarder);
    sawOnEmpty_ = true;
  }

  openmoq::moqx::LocalForwarderRegistry* localReg_;
  moxygen::FullTrackName ftn_;
  std::optional<bool> lastForward_;
  std::optional<uint64_t> maxGroup_;
  bool sawOnEmpty_{false};
};

// Runs on the publisher forwarder's executor (publisher's iothread). Propagates
// channel-subscriber lifecycle events to the publisher and upstream handle.
class ChannelForwarderCallback : public openmoq::moqx::TrackEventCallback {
public:
  ChannelForwarderCallback(ChannelSubscriber channelSub, folly::Executor* subscriberExec)
      : channelSub_(std::move(channelSub)), subscriberExec_(subscriberExec) {}

  // Called on publisherExec immediately after addChannelSubscriber returns.
  void setHandle(std::shared_ptr<moxygen::Publisher::SubscriptionHandle> h) {
    handle_ = std::move(h);
  }

  // Holds the last filter ref so onEmpty can defer its destruction to subscriberExec_,
  // letting in-flight this-capturing lambdas there run first (FIFO).
  void setFilter(std::shared_ptr<CrossExecFilter> filter) { crossExecFilter_ = std::move(filter); }

  void onEmpty(const moxygen::FullTrackName& /*ftn*/) override {
    channelSub_.detach(subscriberExec_);
    // handle_ pins the chain (localFwd → callbacks → handle_ → CrossExecFilter → localFwd).
    // removeChannelSubscriberByExec drops the publisher's ref but not this; in the replace
    // scenario it never ran, so reset handle_ unconditionally.
    handle_.reset();
    // Post filter destruction to subscriberExec_ so FIFO ordering guarantees
    // all previously-enqueued this-capturing lambdas run before the destructor.
    if (crossExecFilter_) {
      subscriberExec_->add([f = std::move(crossExecFilter_)]() {});
    }
  }

  void forwardChanged(const moxygen::FullTrackName&, bool forward) override {
    if (!handle_) {
      return;
    }
    launchUpdate(channelSub_.exec(), doSubscribeUpdate(handle_, forward));
  }

  void newGroupRequested(const moxygen::FullTrackName&, uint64_t group) override {
    if (!handle_) {
      return;
    }
    launchUpdate(channelSub_.exec(), doNewGroupRequestUpdate(handle_, group));
  }

  // publishDone drains the subscribers, so onEmpty follows and does the teardown.
  void onPublishDone(const moxygen::FullTrackName& /*ftn*/) override {}

private:
  ChannelSubscriber channelSub_;
  folly::Executor* subscriberExec_;
  std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle_;
  std::shared_ptr<CrossExecFilter> crossExecFilter_;
};

// Builds the local->publisher callback chain (Channel -> CrossExec -> LocalForwarder).
// channelCb is wired with the channel handle/filter later, on publisherExec.
// Must run on publisherExec: channelCb captures that thread at construction.
struct LocalToPublisherCallbacks {
  std::shared_ptr<ChannelForwarderCallback> channelCb;
  std::shared_ptr<moxygen::MoQForwarder::Callback> finalCallback;
};
LocalToPublisherCallbacks buildLocalToPublisherCallbacks(
    openmoq::moqx::LocalForwarderRegistry* localReg,
    moxygen::FullTrackName ftn,
    ChannelSubscriber channelSub,
    folly::Executor* subscriberExec
) {
  auto* publisherExec = channelSub.exec();
  auto channelCb =
      std::make_shared<ChannelForwarderCallback>(std::move(channelSub), subscriberExec);
  auto crossExecCb = std::make_shared<CrossExecForwarderCallback>(publisherExec, channelCb);
  auto finalCallback =
      std::make_shared<LocalForwarderCallback>(localReg, std::move(ftn), std::move(crossExecCb));
  return {std::move(channelCb), std::move(finalCallback)};
}

// Adds the local channel subscriber on publisherFwd and records its handle/filter on
// channelCb. Must run on publisherExec.
void installChannelSubscriber(
    ChannelForwarderCallback& channelCb,
    moxygen::MoQForwarder& publisherFwd,
    folly::Executor* subscriberExec,
    bool forward,
    const std::shared_ptr<CrossExecFilter>& crossExecFilter
) {
  auto chanHandle = publisherFwd.addChannelSubscriber(subscriberExec, forward, crossExecFilter);
  if (chanHandle) {
    channelCb.setHandle(chanHandle);
  }
  channelCb.setFilter(crossExecFilter);
}

// Installs finalCallback on localFwd and replays any forwardChanged/newGroupRequested
// events captured during setup. Must run on subscriberExec.
void replayPendingFowarderEvents(
    moxygen::MoQForwarder* localFwd,
    const std::shared_ptr<moxygen::MoQForwarder::Callback>& finalCallback,
    const PendingForwarderCallback& pendingCb,
    bool forward
) {
  localFwd->setCallback(finalCallback);
  if (pendingCb.lastForward_ && *pendingCb.lastForward_ != forward) {
    finalCallback->forwardChanged(localFwd, *pendingCb.lastForward_);
  }
  if (pendingCb.maxGroup_) {
    finalCallback->newGroupRequested(localFwd, *pendingCb.maxGroup_);
  }
}
} // namespace

folly::coro::Task<std::optional<MoqxRelay::ResolvedPublisher>>
MoqxRelay::resolvePublisherOnItsExec(TrackRef track) {
  auto* publisherReg = tlForwarders_.get();
  auto publisherFwd = publisherReg ? publisherReg->getIfReady(track.ftn) : nullptr;
  if (!publisherFwd) {
    co_return std::nullopt;
  }
  co_return ResolvedPublisher{
      ForwarderRef::remote(publisherFwd, folly::getKeepAliveToken(track.exec)),
      ChannelSubscriber(publisherFwd, track.exec),
      InitialTrackState::capture(*publisherFwd)
  };
}

// Runs on subscriberExec. Gets or creates the thread-local forwarder, wires it to the publisher
// forwarder as a channel subscriber (isNew path), and awaits the publish reply.
folly::coro::Task<void> MoqxRelay::addSubscriberAndPublishViaLocalForwarder(
    std::shared_ptr<MoQSession> subscriberSession,
    TrackRef track,
    bool forward,
    bool pinned
) {
  auto* subscriberExec = subscriberSession->getExecutor();
  const auto& ftn = track.ftn;
  auto* publisherExec = track.exec;

  // Fast path: local forwarder already exists on this thread.
  if (auto* localReg = tlForwarders_.get()) {
    if (auto localFwd = localReg->getIfReady(ftn)) {
      auto p = startPublish(subscriberSession, localFwd, forward, pinned, nullptr);
      if (p) {
        co_await awaitPublishReply(localFwd, std::move(p->subscriber), std::move(p->reply));
      }
      co_return;
    }
  }

  auto publisher = co_await folly::coro::co_withExecutor(
      folly::getKeepAliveToken(publisherExec),
      resolvePublisherOnItsExec(track)
  );
  if (!publisher) {
    co_return; // torn down before the hop landed
  }

  auto [localFwd, isNew, localReg] = acquireLocalForwarder(ftn, publisher->initial);
  if (!localFwd) {
    // Setup for this track is in flight on this thread; drop the fanout.
    co_return;
  }

  auto p = startPublish(subscriberSession, localFwd, forward, pinned, nullptr);
  if (!p) {
    if (isNew) {
      localReg->remove(ftn, localFwd.get());
    }
    co_return;
  }

  if (!isNew) {
    co_await awaitPublishReply(localFwd, std::move(p->subscriber), std::move(p->reply));
    co_return;
  }

  // isNew=true: wire localFwd into publisherFwd as a channel subscriber.
  auto pendingCb = std::make_shared<PendingForwarderCallback>(localReg, ftn);
  localFwd->setCallback(pendingCb);
  // deepCopyPayload=true (default): each subscriber thread owns its IOBuf chain,
  // avoiding cross-thread contention on the shared atomic refcount.
  auto crossExecFilter = CrossExecFilter::create(subscriberExec, localFwd);
  bool hasForwardingSub = (localFwd->numForwardingSubscribers() > 0);

  // Wire localFwd in as a channel subscriber on the publisher's exec
  auto finalCallback = co_await publisher->ref.co_with([&](MoQForwarder& publisherFwd) {
    auto cbs = buildLocalToPublisherCallbacks(
        localReg,
        ftn,
        std::move(publisher->channelSub),
        subscriberExec
    );
    installChannelSubscriber(
        *cbs.channelCb,
        publisherFwd,
        subscriberExec,
        hasForwardingSub,
        crossExecFilter
    );
    return cbs.finalCallback;
  });

  // Natural unwind: back on subscriberExec.

  // attached=0: the publisher went away before the channel sub was installed.
  // sawOnEmpty=1: every subscriber cancelled while setup was in flight.
  if (!finalCallback || pendingCb->sawOnEmpty_) {
    auto reason = folly::to<std::string>(
        "local forwarder setup failed: attached=",
        finalCallback.has_value(),
        " sawOnEmpty=",
        pendingCb->sawOnEmpty_
    );
    XLOG(ERR) << reason << " ftn=" << ftn;
    teardownLocalForwarderOnFailure(
        publisher->ref,
        subscriberExec,
        /*relayExec=*/nullptr,
        localFwd,
        reason
    );
    co_return;
  }

  replayPendingFowarderEvents(localFwd.get(), *finalCallback, *pendingCb, hasForwardingSub);
  co_await awaitPublishReply(localFwd, std::move(p->subscriber), std::move(p->reply));
}

ForwarderRef MoqxRelay::makeForwarderRef(
    const std::shared_ptr<MoQForwarder>& forwarder,
    folly::Executor* publisherExec
) const {
  if (mode() == Mode::LocalForwarder) {
    return ForwarderRef::remote(forwarder, folly::getKeepAliveToken(publisherExec));
  }
  return ForwarderRef::owned(forwarder);
}

LocalForwarderRegistry& MoqxRelay::localRegistry() {
  if (!tlForwarders_.get()) {
    tlForwarders_.reset(new LocalForwarderRegistry());
  }
  return *tlForwarders_;
}

// Slow-path local-forwarder bootstrap shared by the publish and subscribe LF paths.
// Callers handle the fast path and install the PendingForwarderCallback themselves,
// because the timing differs.
MoqxRelay::LocalForwarderBootstrap
MoqxRelay::acquireLocalForwarder(const FullTrackName& ftn, const InitialTrackState& initial) {
  auto* localReg = &localRegistry();
  auto joined = localReg->join(ftn, [&] { return std::make_shared<MoQForwarder>(ftn); });
  if (auto* claim = std::get_if<LocalForwarderRegistry::Claim>(&joined)) {
    auto localFwd = claim->forwarder();
    claim->markReady(initial);
    return {std::move(localFwd), /*isNew=*/true, localReg};
  }
  auto* ready = std::get_if<LocalForwarderRegistry::Ready>(&joined);
  if (!ready) {
    // A setup on this thread owns the entry and cannot be joined mid-flight. Drop the
    // fanout rather than publishing over a subscribe that is still claiming the track.
    XLOG(WARNING) << "local forwarder setup in flight, dropping publish fanout for " << ftn;
    return {};
  }
  return {ready->forwarder, /*isNew=*/false, localReg};
}

class MoqxRelay::NamespaceSubscription : public Publisher::SubscribeNamespaceHandle {
public:
  NamespaceSubscription(
      std::shared_ptr<MoqxRelay> relay,
      std::shared_ptr<MoQSession> session,
      SubscribeNamespaceOk ok,
      TrackNamespace trackNamespacePrefix
  )
      : Publisher::SubscribeNamespaceHandle(std::move(ok)), relay_(std::move(relay)),
        session_(std::move(session)), trackNamespacePrefix_(std::move(trackNamespacePrefix)) {}

  void unsubscribeNamespace() override {
    if (relay_) {
      relay_->unsubscribeNamespace(trackNamespacePrefix_, std::move(session_));
      relay_.reset();
    }
  }

  folly::coro::Task<RequestUpdateResult> requestUpdate(RequestUpdate reqUpdate) override {
    co_return folly::makeUnexpected(RequestError{
        reqUpdate.requestID,
        RequestErrorCode::NOT_SUPPORTED,
        "REQUEST_UPDATE not supported for relay SUBSCRIBE_NAMESPACE"
    });
  }

private:
  std::shared_ptr<MoqxRelay> relay_;
  std::shared_ptr<MoQSession> session_;
  TrackNamespace trackNamespacePrefix_;
};

// Draft 18+: handle returned from subscribeTracks(). Calls unsubscribeTracks()
// on destruction / explicit cancel, mirroring NamespaceSubscription above.
class MoqxRelay::TracksSubscription : public Publisher::SubscribeTracksHandle {
public:
  TracksSubscription(
      std::shared_ptr<MoqxRelay> relay,
      std::shared_ptr<MoQSession> session,
      RequestOk ok,
      TrackNamespace trackNamespacePrefix
  )
      : Publisher::SubscribeTracksHandle(std::move(ok)), relay_(std::move(relay)),
        session_(std::move(session)), trackNamespacePrefix_(std::move(trackNamespacePrefix)) {}

  void unsubscribeTracks() override {
    if (relay_) {
      relay_->unsubscribeTracks(trackNamespacePrefix_, std::move(session_));
      relay_.reset();
    }
  }

  folly::coro::Task<RequestUpdateResult> requestUpdate(RequestUpdate reqUpdate) override {
    // Draft-18 Section 10.9.2 allows REQUEST_UPDATE for SUBSCRIBE_TRACKS to update
    // the TRACK_NAMESPACE_PREFIX parameter. However, moxygen's MoQSession::onRequestUpdate()
    // only routes REQUEST_UPDATE to SUBSCRIBE and FETCH handles, not SUBSCRIBE_TRACKS.
    // This is a moxygen limitation; when fixed upstream, we can implement prefix updates here
    // by removing and re-adding the subscription with the new prefix, checking for overlaps.
    co_return folly::makeUnexpected(RequestError{
        reqUpdate.requestID,
        RequestErrorCode::NOT_SUPPORTED,
        "REQUEST_UPDATE not supported for relay SUBSCRIBE_TRACKS (moxygen limitation)"
    });
  }

private:
  std::shared_ptr<MoqxRelay> relay_;
  std::shared_ptr<MoQSession> session_;
  TrackNamespace trackNamespacePrefix_;
};

namespace {

// Records ingested objects for one subgroup; the group ID is fixed at
// beginSubgroup, so only the object ID varies per call.
class RelayIngestSubgroupFilter : public moxygen::SubgroupConsumerFilter {
public:
  RelayIngestSubgroupFilter(
      std::shared_ptr<IngestCounters> ingest,
      uint64_t groupID,
      std::shared_ptr<SubgroupConsumer> downstream
  )
      : moxygen::SubgroupConsumerFilter(std::move(downstream)), ingest_(std::move(ingest)),
        groupID_(groupID) {}

  folly::Expected<folly::Unit, MoQPublishError> object(
      uint64_t objectID,
      Payload payload,
      moxygen::Extensions extensions = moxygen::noExtensions(),
      bool finSubgroup = false
  ) override {
    ingest_->record(groupID_, objectID);
    return moxygen::SubgroupConsumerFilter::object(
        objectID,
        std::move(payload),
        std::move(extensions),
        finSubgroup
    );
  }

  folly::Expected<folly::Unit, MoQPublishError> beginObject(
      uint64_t objectID,
      uint64_t length,
      Payload initialPayload,
      moxygen::Extensions extensions = moxygen::noExtensions()
  ) override {
    ingest_->record(groupID_, objectID);
    return moxygen::SubgroupConsumerFilter::beginObject(
        objectID,
        length,
        std::move(initialPayload),
        std::move(extensions)
    );
  }

  // endOfGroup/endOfTrackAndGroup deliver a real status object, so they count.
  folly::Expected<folly::Unit, MoQPublishError> endOfGroup(uint64_t endOfGroupObjectID) override {
    ingest_->record(groupID_, endOfGroupObjectID);
    return moxygen::SubgroupConsumerFilter::endOfGroup(endOfGroupObjectID);
  }

  folly::Expected<folly::Unit, MoQPublishError> endOfTrackAndGroup(uint64_t endOfTrackObjectID
  ) override {
    ingest_->record(groupID_, endOfTrackObjectID);
    return moxygen::SubgroupConsumerFilter::endOfTrackAndGroup(endOfTrackObjectID);
  }

private:
  std::shared_ptr<IngestCounters> ingest_;
  uint64_t groupID_;
};

} // namespace

// The relay executor's per-object observation point on the ingest path: counts
// what arrives for /state, and intercepts publishDone to clean up relay state.
// Both buildFilterChain branches install one, so these counters are the only
// source /state needs -- in LocalForwarder mode the forwarder itself is owned
// by another executor and cannot be read during the walk.
//
// Holds a weak_ptr to avoid a reference cycle: relay owns RelaySubscription
// which owns the filter chain (TopNFilter→RelayIngestFilter), so a strong
// relay ref here would prevent the relay from ever being destroyed.
class MoqxRelay::RelayIngestFilter : public TrackConsumerFilter {
public:
  RelayIngestFilter(
      std::weak_ptr<MoqxRelay> relay,
      FullTrackName ftn,
      std::shared_ptr<IngestCounters> ingest,
      std::shared_ptr<TrackConsumer> downstream
  )
      : TrackConsumerFilter(std::move(downstream)), relay_(std::move(relay)), ftn_(std::move(ftn)),
        ingest_(std::move(ingest)) {}

  folly::Expected<std::shared_ptr<SubgroupConsumer>, MoQPublishError> beginSubgroup(
      uint64_t groupID,
      uint64_t subgroupID,
      moxygen::Priority priority,
      moxygen::BeginSubgroupOptions options = {}
  ) override {
    auto res = TrackConsumerFilter::beginSubgroup(groupID, subgroupID, priority, options);
    if (!res) {
      return res;
    }
    return std::static_pointer_cast<SubgroupConsumer>(
        std::make_shared<RelayIngestSubgroupFilter>(ingest_, groupID, std::move(res.value()))
    );
  }

  folly::Expected<folly::Unit, MoQPublishError>
  objectStream(const ObjectHeader& header, Payload payload, bool lastInGroup = false) override {
    ingest_->record(header.group, header.id);
    return TrackConsumerFilter::objectStream(header, std::move(payload), lastInGroup);
  }

  folly::Expected<folly::Unit, MoQPublishError>
  datagram(const ObjectHeader& header, Payload payload, bool lastInGroup = false) override {
    ingest_->record(header.group, header.id);
    return TrackConsumerFilter::datagram(header, std::move(payload), lastInGroup);
  }

  folly::Expected<folly::Unit, MoQPublishError> publishDone(PublishDone pubDone) override {
    // Notify relay that publisher is done - this will:
    // 1. Remove from nodePtr->publishes
    // 2. Clear subscription.handle
    if (auto relay = relay_.lock()) {
      relay->onPublishDone(ftn_);
    }
    // Change the downstream code to something like "upstream ended"?
    return TrackConsumerFilter::publishDone(std::move(pubDone));
  }

private:
  std::weak_ptr<MoqxRelay> relay_;
  FullTrackName ftn_;
  std::shared_ptr<IngestCounters> ingest_;
};

SubscriptionRegistry::FilterChainResult
MoqxRelay::buildFilterChain(const FullTrackName& ftn, std::shared_ptr<MoQForwarder> forwarder) {
  if (mode() == Mode::LocalForwarder) {
    // Multi-iothread with local forwarders: publisher writes directly to forwarder on
    // publisherExec. relayChainFilter (added by publish()) fans off to
    // topNFilter/termination/cache.
    std::shared_ptr<TrackConsumer> chainEnd =
        cache_ ? cache_->makePassiveConsumer(ftn) : std::make_shared<moxygen::NullTrackConsumer>();
    auto ingest = std::make_shared<IngestCounters>();
    auto ingestFilter =
        std::make_shared<RelayIngestFilter>(shared_from_this(), ftn, ingest, std::move(chainEnd));
    auto topNFilter =
        std::make_shared<TopNFilter>(ftn, std::static_pointer_cast<TrackConsumer>(ingestFilter));
    topNFilter->setActivityThreshold(activityThreshold_);
    return SubscriptionRegistry::FilterChainResult{
        .consumer = std::static_pointer_cast<TrackConsumer>(forwarder),
        .topNFilter = topNFilter,
        .chainHead = wrapWithTrackStats(
            trackStats_,
            ftn,
            std::static_pointer_cast<TrackConsumer>(topNFilter),
            stats::TrackDirection::Ingest
        ),
        .ingest = std::move(ingest)
    };
  }

  // Single-threaded: chain wraps forwarder directly (no cross-exec needed).
  // Cache attaches as a passive subscriber of the forwarder.
  if (cache_) {
    forwarder->addSubscriber(
        /*session=*/nullptr,
        /*forward=*/true,
        cache_->makePassiveConsumer(ftn),
        /*passive=*/true
    );
  }
  auto ingest = std::make_shared<IngestCounters>();
  auto ingestFilter = std::make_shared<RelayIngestFilter>(
      shared_from_this(),
      ftn,
      ingest,
      std::static_pointer_cast<TrackConsumer>(forwarder)
  );
  auto topNFilter =
      std::make_shared<TopNFilter>(ftn, std::static_pointer_cast<TrackConsumer>(ingestFilter));
  topNFilter->setActivityThreshold(activityThreshold_);
  auto chainHead = wrapWithTrackStats(
      trackStats_,
      ftn,
      std::static_pointer_cast<TrackConsumer>(topNFilter),
      stats::TrackDirection::Ingest
  );
  return SubscriptionRegistry::FilterChainResult{
      .consumer = chainHead,
      .topNFilter = topNFilter,
      .chainHead = chainHead,
      .ingest = std::move(ingest)
  };
}

folly::coro::Task<Publisher::SubscribeNamespaceResult> MoqxRelay::subscribeNamespace(
    SubscribeNamespace subNs,
    std::shared_ptr<NamespacePublishHandle> namespacePublishHandle
) {
  return subscribeNamespaceImpl(std::move(subNs), std::move(namespacePublishHandle));
}

folly::coro::Task<Publisher::SubscribeNamespaceResult> MoqxRelay::subscribeNamespaceImpl(
    SubscribeNamespace subNs,
    std::shared_ptr<NamespacePublishHandle> namespacePublishHandle
) {
  XLOG(DBG1) << __func__ << " nsp=" << subNs.trackNamespacePrefix;

  auto session = MoQSession::getRequestSession();

  // Relay peering: if the incoming subNs carries a relay auth token, the peer
  // is a relay. Reciprocate with our own peer subNs so the peer gets our
  // namespace announcements as publishers connect.
  std::string incomingPeerID;
  if (auto peerID = !relayID_.empty() ? getPeerRelayID(subNs) : std::nullopt) {
    incomingPeerID = *peerID;
    XLOG(INFO) << __func__ << ": peer relay detected peer_id=" << *peerID
               << ", reciprocating peer subNs";
    // Tag with the peer's relay ID so we suppress echoing these namespaces
    // back to that peer on reconnect.
    auto handle = makeNamespaceBridgeHandle(weak_from_this(), session, incomingPeerID, relayExec_);
    auto peerSubNs = makePeerSubNs();
    if (session->negotiatedSetupExtension(SetupExtension::RelayHops)) {
      peerSubNs.params.insertParam(
          Parameter(folly::to_underlying(TrackRequestParamKey::EXCLUDE_HOP), relayHopID_)
      );
    }
    // maybeWrapPublisher runs the call on the peer session's executor and wraps
    // the returned handle so its teardown hops there too (no token: reciprocal).
    auto recipResult = co_await maybeWrapPublisher(relayExec_, session)
                           ->subscribeNamespace(std::move(peerSubNs), handle);
    if (recipResult.hasError()) {
      XLOG(ERR) << "Reciprocal peer subNs failed: " << recipResult.error().reasonPhrase;
    } else {
      peerSubNsHandles_.emplace(
          session.get(),
          PeerInfo{std::move(recipResult.value()), std::move(*peerID)}
      );
    }
    // Fall through: register the peer as a normal subNs subscriber so it
    // receives namespace announcements as publishers connect.
  }
  auto maybeNegotiatedVersion = session->getNegotiatedVersion();
  CHECK(maybeNegotiatedVersion.has_value());

  // Allow empty namespace prefix only for draft-16 and above.
  if (subNs.trackNamespacePrefix.empty() && getDraftMajorVersion(*maybeNegotiatedVersion) < 16) {
    co_return folly::makeUnexpected(SubscribeNamespaceError{
        subNs.requestID,
        SubscribeNamespaceErrorCode::NAMESPACE_PREFIX_UNKNOWN,
        "empty"
    });
  }
  SubscribeNamespaceOptions effectiveOptions;
  effectiveOptions = subNs.options;

  // Parse parameters defined for SUBSCRIBE_NAMESPACE.
  std::optional<TrackFilter> trackFilter;
  std::optional<uint64_t> excludeHop;
  if (const auto* param = subNs.params.getFirstParam(TrackRequestParamKey::TRACK_FILTER)) {
    trackFilter = param->asTrackFilter;
  }
  if (session->negotiatedSetupExtension(SetupExtension::RelayHops)) {
    if (const auto* param = subNs.params.getFirstParam(TrackRequestParamKey::EXCLUDE_HOP)) {
      excludeHop = param->asUint64;
    }
  }

  auto nodePtr = namespaceTree_.addNamespaceSubscriber(
      subNs.trackNamespacePrefix,
      session,
      NamespaceTree::NamespaceNode::NamespaceSubscriberInfo{
          subNs.forward,
          effectiveOptions,
          namespacePublishHandle,
          subNs.trackNamespacePrefix,
          trackFilter,
          excludeHop
      }
  );

  // If TRACK_FILTER is present, enroll session in PropertyRanking for top-N selection.
  // NOTE: onSelected callbacks fire synchronously within addSessionToTopNGroup() for
  // tracks already in top-N, triggering onTrackSelected() before this call returns.
  if (trackFilter) {
    auto ranking =
        getOrCreateRanking(nodePtr, trackFilter->propertyType, subNs.trackNamespacePrefix);
    ranking->addSessionToTopNGroup(trackFilter->maxSelected, session, subNs.forward);
  }

  // Find all nested PublishNamespaces/Publishes and forward
  auto exec = session->getExecutor();
  namespaceTree_.forEachNodeInSubtree(
      subNs.trackNamespacePrefix,
      nodePtr,
      [&](const TrackNamespace& prefix, std::shared_ptr<NamespaceTree::NamespaceNode> node) {
        if (node->publisherSession() &&
            (incomingPeerID.empty() || node->publisherPeerID() != incomingPeerID) &&
            shouldForwardNamespace(
                node->publisherSession(),
                session,
                subNs.options,
                excludeHop,
                node->relayHopPath(),
                relayHopID_
            )) {
          if (getDraftMajorVersion(*maybeNegotiatedVersion) >= 16) {
            if (subNs.options == SubscribeNamespaceOptions::NAMESPACE ||
                subNs.options == SubscribeNamespaceOptions::BOTH) {
              // Compute the suffix: prefix minus subNs.trackNamespacePrefix
              auto suffix = makeNamespaceSuffix(prefix, subNs.trackNamespacePrefix.size());
              Namespace ns;
              ns.trackNamespaceSuffix = std::move(suffix);
              setOutgoingHopPath(ns.params, session, node->relayHopPath(), relayHopID_);
              namespacePublishHandle->namespaceMsg(ns);
            }
          } else {
            // TODO: Auth/params
            PublishNamespace pubNs{subNs.requestID, prefix};
            setOutgoingHopPath(pubNs.params, session, node->relayHopPath(), relayHopID_);
            co_withExecutor(exec, publishNamespaceToSession(session, std::move(pubNs), node))
                .start();
          }
        }
        node->forEachPublish([&](const std::string& trackName,
                                 const std::shared_ptr<MoQSession>& publishSession) {
          FullTrackName ftn{prefix, trackName};
          auto forwarder = registry_.getForwarderRef(ftn);
          if (!forwarder) {
            XLOG(ERR) << "Invalid state, no subscription for publish ftn=" << ftn;
            return;
          }
          auto maybeNegotiatedVersion = session->getNegotiatedVersion();
          CHECK(maybeNegotiatedVersion.has_value());

          // TRACK_FILTER subscribers: PropertyRanking drives selection via
          // onTrackSelected; skip direct publish here.
          if (trackFilter) {
            return;
          }

          if (getDraftMajorVersion(*maybeNegotiatedVersion) <= 15 ||
              (subNs.options == SubscribeNamespaceOptions::BOTH ||
               subNs.options == SubscribeNamespaceOptions::PUBLISH)) {
            if (publishSession != session) {
              if (!addSubscriberAndPublish(session, forwarder, subNs.forward, /*pinned=*/true)) {
                XLOG(ERR) << "addSubscriberAndPublish failed for " << ftn;
                return;
              }
            }
          }
        });
      }
  );
  co_return std::make_shared<NamespaceSubscription>(
      shared_from_this(),
      std::move(session),
      SubscribeNamespaceOk{.requestID = subNs.requestID, .requestSpecificParams = {}},
      subNs.trackNamespacePrefix
  );
}

void MoqxRelay::unsubscribeNamespace(
    const TrackNamespace& trackNamespacePrefix,
    std::shared_ptr<MoQSession> session
) {
  XLOG(DBG1) << __func__ << " nsp=" << trackNamespacePrefix;
  // Clean up the reciprocal peer subNs handle for this session if present.
  peerSubNsHandles_.erase(session.get());
  auto result = namespaceTree_.removeNamespaceSubscriber(trackNamespacePrefix, session);
  if (result.hasError() && result.error() == NamespaceTree::Error::NotSubscribed) {
    XLOG(DBG1) << "Namespace prefix was not subscribed by this session";
  }
}

// Draft 18+
folly::coro::Task<Publisher::SubscribeTracksResult> MoqxRelay::subscribeTracks(
    SubscribeTracks subTracks,
    std::shared_ptr<PublishBlockedHandle> /*publishBlockedHandle*/
) {
  XLOG(DBG1) << __func__ << " nsp=" << subTracks.trackNamespacePrefix;

  auto session = MoQSession::getRequestSession();
  auto maybeNegotiatedVersion = session->getNegotiatedVersion();
  XCHECK(maybeNegotiatedVersion.has_value());
  if (getDraftMajorVersion(*maybeNegotiatedVersion) < 18) {
    co_return folly::makeUnexpected(SubscribeTracksError{
        subTracks.requestID,
        SubscribeTracksErrorCode::NOT_SUPPORTED,
        "SUBSCRIBE_TRACKS requires draft 18+"
    });
  }

  if (tracksTree_.hasOverlappingTracksSubscription(subTracks.trackNamespacePrefix, session)) {
    co_return folly::makeUnexpected(SubscribeTracksError{
        subTracks.requestID,
        SubscribeTracksErrorCode::PREFIX_OVERLAP,
        "Overlapping SUBSCRIBE_TRACKS exists in this session"
    });
  }

  // Register in the parallel tracks tree (independent overlap space).
  // Tracks-tree entries always behave like PUBLISH-style subscribers;
  // options is unused for this tree.
  tracksTree_.addNamespaceSubscriber(
      subTracks.trackNamespacePrefix,
      session,
      NamespaceTree::NamespaceNode::NamespaceSubscriberInfo{
          subTracks.forward,
          SubscribeNamespaceOptions::PUBLISH,
          /*namespacePublishHandle=*/nullptr,
          subTracks.trackNamespacePrefix,
          /*trackFilter=*/std::nullopt,
          /*excludeHop=*/std::nullopt
      }
  );

  // Walk the existing publish tree and emit PUBLISH for each matching
  // already-published track (backfill for new subscriber).
  auto pubNode = namespaceTree_.findNode(
      subTracks.trackNamespacePrefix,
      /*createMissingNodes=*/false,
      NamespaceTree::MatchType::Exact
  );
  if (pubNode) {
    namespaceTree_.forEachNodeInSubtree(
        subTracks.trackNamespacePrefix,
        pubNode,
        [&](const TrackNamespace& prefix, std::shared_ptr<NamespaceTree::NamespaceNode> node) {
          node->forEachPublish([&](const std::string& trackName,
                                   const std::shared_ptr<MoQSession>& publishSession) {
            if (publishSession == session) {
              // Don't echo the subscriber's own published tracks.
              return;
            }
            FullTrackName ftn{prefix, trackName};
            auto forwarder = registry_.getForwarderRef(ftn);
            if (!forwarder) {
              return;
            }
            if (!addSubscriberAndPublish(session, forwarder, subTracks.forward, /*pinned=*/true)) {
              XLOG(ERR) << "addSubscriberAndPublish failed for " << ftn;
              return;
            }
          });
        }
    );
  }

  RequestOk subTracksOk{.requestID = subTracks.requestID};
  co_return std::make_shared<TracksSubscription>(
      shared_from_this(),
      std::move(session),
      std::move(subTracksOk),
      subTracks.trackNamespacePrefix
  );
}

void MoqxRelay::unsubscribeTracks(
    const TrackNamespace& trackNamespacePrefix,
    std::shared_ptr<MoQSession> session
) {
  XLOG(DBG1) << __func__ << " nsp=" << trackNamespacePrefix;
  auto result = tracksTree_.removeNamespaceSubscriber(trackNamespacePrefix, session);
  if (result.hasError() && result.error() == NamespaceTree::Error::NotSubscribed) {
    XLOG(DBG1) << "Tracks prefix was not subscribed by this session";
  }
}

MoqxRelay::PublishState MoqxRelay::findPublishState(const FullTrackName& ftn) {
  PublishState state;
  auto nodePtr = namespaceTree_.findNode(
      ftn.trackNamespace,
      /*createMissingNodes=*/false,
      NamespaceTree::MatchType::Exact
  );

  if (!nodePtr) {
    return state;
  }

  state.nodeExists = true;

  state.session = nodePtr->findPublishSession(ftn.trackName);

  return state;
}

// === Multi-iothread subscribe helpers ===

namespace {

// The error returned when addSubscriber yields null (forwarder draining). Shared
// by every subscribe path that attaches to a live forwarder; callers wrap it with
// folly::makeUnexpected for their SubscribeResult.
SubscribeError makeAddSubscriberError(RequestID requestID) {
  return SubscribeError{requestID, SubscribeErrorCode::INTERNAL_ERROR, "failed to add subscriber"};
}

// Adds a subscriber to fwd, fires any pending NEW_GROUP_REQUEST, and maps a null
// result (forwarder draining) to INTERNAL_ERROR. Shared by the subscribe paths
// that attach to an already-live forwarder.
Publisher::SubscribeResult attachSubscriber(
    MoQForwarder& fwd,
    std::shared_ptr<MoQSession> session,
    const SubscribeRequest& subReq,
    std::shared_ptr<TrackConsumer> consumer
) {
  auto subscriber = fwd.addSubscriber(std::move(session), subReq, std::move(consumer));
  if (!subscriber) {
    XLOG(ERR) << "addSubscriber returned null (draining?) for " << fwd.fullTrackName()
              << " reqID=" << subReq.requestID;
    return folly::makeUnexpected(makeAddSubscriberError(subReq.requestID));
  }
  XLOG(DBG4) << "added subscriber for ftn=" << fwd.fullTrackName();
  fwd.tryProcessNewGroupRequest(subReq.params);
  return subscriber;
}

} // namespace

// Issues the upstream SUBSCRIBE on `upstream` and, on success, applies the OK to
// publisherFwd (latest/extensions/NGR), returning the resolved OK. forward must
// already be set on upstreamSubReq.
folly::coro::Task<folly::Expected<MoqxRelay::UpstreamOk, SubscribeError>>
MoqxRelay::subscribeUpstreamAndApplyOk(
    std::shared_ptr<Publisher> upstream,
    SubscribeRequest upstreamSubReq,
    std::shared_ptr<TrackConsumer> upstreamConsumer,
    std::shared_ptr<MoQForwarder> publisherFwd,
    RequestID clientRequestID
) {
  auto params = upstreamSubReq.params; // copy before upstreamSubReq is moved
  auto subRes =
      co_await upstream->subscribe(std::move(upstreamSubReq), std::move(upstreamConsumer));
  if (subRes.hasError()) {
    co_return folly::makeUnexpected(SubscribeError{
        clientRequestID,
        subRes.error().errorCode,
        folly::to<std::string>("upstream subscribe failed: ", subRes.error().reasonPhrase)
    });
  }
  // Apply the OK to the forwarder; the NGR rides the outgoing SUBSCRIBE (record, don't fire).
  const auto& ok = subRes.value()->subscribeOk();
  InitialTrackState{ok.largest, ok.extensions}.applyTo(*publisherFwd);
  publisherFwd->tryProcessNewGroupRequest(params, /*fire=*/false);
  // Moving the handle shared_ptr keeps the pointee (and `ok`) alive, so reading ok.*
  // in the same initializer is well-defined.
  co_return UpstreamOk{std::move(subRes.value()), ok.requestID, ok.extensions, ok.largest};
}

// Runs on relayExec_: caches the OK's extensions and fulfills `pending`, returning a
// SubscribeError on reconnect (pending replaced) or nullopt. Shared by subscribeImpl
// and the LF path; the caller applies upstreamOk.largest (target differs per path).
std::optional<SubscribeError> MoqxRelay::completeUpstreamSubscription(
    const FullTrackName& ftn,
    UpstreamOk& upstreamOk,
    SubscriptionRegistry::UpstreamSubscribePending& pending,
    std::shared_ptr<MoQSession> upstreamSession,
    std::shared_ptr<Publisher> upstreamPublisher,
    RequestID clientRequestID
) {
  if (cache_) {
    cache_->setTrackExtensions(ftn, upstreamOk.extensions);
  }
  if (!pending.complete(
          std::move(upstreamOk.handle),
          upstreamOk.requestID,
          std::move(upstreamSession),
          std::move(upstreamPublisher)
      )) {
    XLOG(ERR) << "Subscription replaced by reconnecting publisher: " << ftn;
    return SubscribeError{
        clientRequestID,
        SubscribeErrorCode::INTERNAL_ERROR,
        "publisher reconnected during subscribe"
    };
  }
  return std::nullopt;
}

// Runs on relayExec_. Registers the subscribe and wires the local forwarder to the
// publisher; if it's a new subscription,also installs the passive relay chain, issues the
// upstream subscribe, and completes the setup. The subscriberExec tail reads the
// returned PublisherAttachment.
folly::coro::Task<MoqxRelay::PublisherAttachment> MoqxRelay::attachNewLocalForwarderOnRelayExec(
    const SubscribeRequest& subReq,
    LocalForwarderRegistry* localReg,
    std::shared_ptr<MoQForwarder> localFwd,
    folly::Executor* subscriberExec,
    std::shared_ptr<CrossExecFilter> crossExecFilter,
    bool forward
) {
  // Runs on relayExec_.
  const auto& ftn = subReq.fullTrackName;
  PublisherAttachment attach;

  auto sr = co_await joinOrPrepareUpstreamSubscription(subReq);
  if (sr.error) {
    attach.error = std::move(*sr.error);
    co_return attach; // pending dtor fires when sr is destroyed, cleaning the registry
  }
  auto* publisherExec = sr.publisherExec;
  if (!publisherExec) {
    co_return attach;
  }
  std::shared_ptr<CrossExecFilter> relayChainFilter;
  std::optional<folly::Expected<UpstreamOk, SubscribeError>> upstreamResult;

  // One publisherExec sortie to setup the publisher forwarder on its exec.
  // The first subscriber transfers ownership to the local registry, installs the
  // relay chain, and subscribes upstream.  All subscribers install their channel
  // subscriber, and capture the initialState.
  co_await folly::coro::co_withExecutor(
      folly::getKeepAliveToken(publisherExec),
      [&]() -> folly::coro::Task<void> {
        std::shared_ptr<MoQForwarder> publisherFwd;
        const char* failure = nullptr;
        if (sr.firstSetup) {
          auto* occupant = localRegistry().getForEviction(ftn).get();
          if (occupant && occupant != localFwd.get()) {
            // A PUBLISH here raced a SUBSCRIBE from another thread.
            // TODO: join the publisher forwarder instead of failing the SUBSCRIBE.
            failure = "publisher forwarder already installed";
          } else {
            publisherFwd = std::move(sr.firstSetup->publisherForwarder);
          }
        } else {
          // Could be displaced by a reconnecting publisher, or the publisher could have gone away
          publisherFwd = localRegistry().getIfReady(ftn);
          if (!publisherFwd) {
            failure = "publisher forwarder gone";
          }
        }
        if (failure) {
          XLOG(ERR) << failure << " during subscribe setup: " << ftn;
          attach.error = folly::makeUnexpected(
              SubscribeError{subReq.requestID, SubscribeErrorCode::INTERNAL_ERROR, failure}
          );
          co_return;
        }
        attach.publisherRef =
            ForwarderRef::remote(publisherFwd, folly::getKeepAliveToken(publisherExec));

        LocalForwarderRegistry::Claim publisherClaim;
        if (sr.firstSetup) {
          publisherClaim = std::move(
              installPublisherForwarder(ftn, publisherFwd, InstallKind::FromSubscribe).claim
          );
        }

        auto cbs = buildLocalToPublisherCallbacks(
            localReg,
            ftn,
            ChannelSubscriber(publisherFwd, publisherExec),
            subscriberExec
        );
        attach.finalCallback = cbs.finalCallback;

        installChannelSubscriber(
            *cbs.channelCb,
            *publisherFwd,
            subscriberExec,
            forward,
            crossExecFilter
        );

        if (sr.firstSetup) {
          auto& setup = *sr.firstSetup;
          // Passive relay chain (top-N/termination/cache): forward=true so it observes every
          // object, passive=true so it doesn't count as a forwarding subscriber or in the
          // onEmpty quorum (the publisher's onEmpty still fires when the last real sub leaves).
          relayChainFilter = CrossExecFilter::create(relayExec_, nullptr);
          publisherFwd->addChannelSubscriber(
              relayExec_,
              /*forward=*/true,
              relayChainFilter,
              /*passive=*/true
          );
          attach.ownsRelayChain = true;
          setup.upstreamSubReq.forward = forward;
          upstreamResult = co_await subscribeUpstreamAndApplyOk(
              setup.upstreamSession,
              std::move(setup.upstreamSubReq),
              std::move(setup.upstreamConsumer),
              publisherFwd,
              setup.clientRequestID
          );
          if (upstreamResult->hasValue()) {
            const auto& ok = upstreamResult->value();
            publisherClaim.markReady(InitialTrackState{ok.largest, ok.extensions});
          }
        }
        // Capture largest/extensions on publisherExec to set the initial state for the
        // subscriber.  Captured but ignored when the firstSetup returned an error.
        attach.initial = InitialTrackState::capture(*publisherFwd);
      }()
  );
  // Back on relayExec_.

  if (attach.error) {
    co_return attach; // subsequent resolve failed in the sortie
  }
  if (!sr.firstSetup) {
    co_return attach; // subsequent subscriber: wired to the live publisher, done
  }

  if (upstreamResult->hasError()) {
    attach.error = folly::makeUnexpected(std::move(upstreamResult->error()));
    co_return attach;
  }
  auto upstreamOk = std::move(upstreamResult->value());

  // Wire the relay chain before pending.complete() so buffered objects see the filters.
  if (relayChainFilter) {
    auto topNView = registry_.getTopNView(ftn);
    if (topNView && topNView->chainHead) {
      relayChainFilter->setDownstream(topNView->chainHead);
    }
  }

  auto& setup = *sr.firstSetup;
  if (auto err = completeUpstreamSubscription(
          ftn,
          upstreamOk,
          setup.pending,
          setup.upstreamSession,
          maybeWrapPublisher(relayExec_, setup.upstreamSession),
          setup.clientRequestID
      )) {
    // Reconnect race: matches prior behavior — no channel-sub teardown here; the tail
    // drains localFwd, and the replaced registry entry already owns the publisher.
    attach.error = folly::makeUnexpected(std::move(*err));
    co_return attach;
  }
  co_return attach;
}

// Runs on relayExec_: registry lookup + FirstSubscriber setup. Defers the upstream
// subscribe to attachNewLocalForwarderOnRelayExec (issued from its publisherExec sortie,
// after the channel subs are installed); returns firstSetup for it to complete.
folly::coro::Task<MoqxRelay::StatefulSubscribeResult>
MoqxRelay::joinOrPrepareUpstreamSubscription(SubscribeRequest subReq) {
  const auto& ftn = subReq.fullTrackName;

  if (!registry_.exists(ftn) && upstream_ &&
      !namespaceTree_.findPublisherSession(ftn.trackNamespace)) {
    co_await upstream_->waitForConnected(kUpstreamConnectWaitTimeout);
  }

  // upstreamSession is set by the factory for the first subscriber that needs to go upstream.
  std::shared_ptr<MoQSession> upstreamSession;
  auto firstOrSubsequent = registry_.getOrCreateFromSubscribe(
      ftn,
      // installPublisherForwarder puts the real chain on the forwarder's own exec instead;
      // a relay-direct callback would run there and touch registry_ off relayExec_.
      std::shared_ptr<MoQForwarder::Callback>(nullptr),
      [this, &ftn](std::shared_ptr<MoQForwarder> f) { return buildFilterChain(ftn, std::move(f)); },
      [this, &ftn, &upstreamSession](const std::shared_ptr<MoQForwarder>& f
      ) -> std::optional<ForwarderRef> {
        upstreamSession = namespaceTree_.findPublisherSession(ftn.trackNamespace);
        if (!upstreamSession) {
          return std::nullopt;
        }
        return makeForwarderRef(f, upstreamSession->getExecutor());
      }
  );

  if (std::get_if<SubscriptionRegistry::NoPublisher>(&firstOrSubsequent)) {
    co_return StatefulSubscribeResult{
        nullptr,
        folly::makeUnexpected(SubscribeError{
            subReq.requestID,
            SubscribeErrorCode::DOES_NOT_EXIST,
            "no such namespace or track"
        }),
        std::nullopt
    };
  }

  if (auto* first = std::get_if<SubscriptionRegistry::FirstSubscriber>(&firstOrSubsequent)) {
    const auto clientRequestID = subReq.requestID;
    // forward updated to its real value in attachNewLocalForwarderOnRelayExec.
    SubscribeRequest upstreamSubReq = makeUpstreamSubReq(subReq, /*forward=*/false);

    // first->consumer is the publisher forwarder (lives on publisherExec == upstreamSession's
    // executor). No cross-exec wrapping — upstream delivers on that executor directly.
    auto upstreamConsumer = first->consumer;
    StatefulSubscribeResult result{upstreamSession->getExecutor(), std::nullopt, std::nullopt};
    result.firstSetup.emplace(StatefulSubscribeResult::FirstSubscriberSetup{
        first->forwarder,
        upstreamSession,
        std::move(upstreamSubReq),
        std::move(upstreamConsumer),
        std::move(first->pending),
        clientRequestID
    });
    co_return result;

  } else {
    // Wait for the first subscriber to complete setup
    auto sub = co_await std::get<folly::coro::Task<SubscriptionRegistry::SubsequentSubscriber>>(
        std::move(firstOrSubsequent)
    );
    auto upstreamView = registry_.getUpstreamView(ftn);
    auto* publisherExec = upstreamView ? upstreamView->publisherExec : nullptr;
    // Return the publisherExec to the subscriber thread to complete setup.
    co_return StatefulSubscribeResult{publisherExec, std::nullopt, std::nullopt};
  }
}

// Multi-iothread subscribe: subscriber-thread orchestrator.
// Dispatches attachNewLocalForwarderOnRelayExec to relayExec_, then on the subscriber thread
// creates a local forwarder, wires a channel subscriber, and returns.
folly::coro::Task<Publisher::SubscribeResult> MoqxRelay::subscribeFromSubscriberExec(
    SubscribeRequest subReq,
    std::shared_ptr<TrackConsumer> consumer,
    std::shared_ptr<MoQSession> session,
    folly::Executor* subscriberExec
) {
  const auto& ftn = subReq.fullTrackName;

  // Join before the relay hop: serializes same-iothread races.
  auto* localReg = &localRegistry();
  auto joined = localReg->join(ftn, [&] { return std::make_shared<MoQForwarder>(ftn); });

  consumer =
      wrapWithTrackStats(trackStats_, ftn, std::move(consumer), stats::TrackDirection::Egress);

  if (auto* pending = std::get_if<LocalForwarderRegistry::Pending>(&joined)) {
    // Another subscriber owns setup. Wait for it.
    co_await folly::coro::co_awaitTry(std::move(pending->ready));
    // Re-resolve: the entry may have been displaced while we were waiting. A failed setup
    // fails this subscriber too.
    auto ready = localReg->getIfReady(ftn);
    if (!ready) {
      co_return folly::makeUnexpected(SubscribeError{
          subReq.requestID,
          SubscribeErrorCode::INTERNAL_ERROR,
          "local forwarder setup failed"
      });
    }
    joined = LocalForwarderRegistry::Ready{std::move(ready)};
  }

  if (auto* ready = std::get_if<LocalForwarderRegistry::Ready>(&joined)) {
    auto& readyFwd = *ready->forwarder;
    if (auto err = checkRangeNotInPast(readyFwd, subReq)) {
      co_return folly::makeUnexpected(std::move(*err));
    }
    co_return attachSubscriber(readyFwd, std::move(session), subReq, std::move(consumer));
  }

  // This thread owns setup. The claim stays open until the tail, so same-thread attachers
  // wait on it also. The Claim destructor will clear the entry and wake subscribers
  // waiting for it unless markReady() is called.
  auto claim = std::move(std::get<LocalForwarderRegistry::Claim>(joined));
  auto localFwd = claim.forwarder();

  // Install PendingForwarderCallback first so forwardChanged/newGroupRequested/onEmpty
  // events during setup are captured for replay.
  auto pendingCb = std::make_shared<PendingForwarderCallback>(localReg, ftn);
  localFwd->setCallback(pendingCb);

  // deepCopyPayload=true (default): each subscriber thread owns its IOBuf chain,
  // avoiding cross-thread contention on the shared atomic refcount.
  auto crossExecFilter = CrossExecFilter::create(subscriberExec, localFwd);

  // addSubscriber before the relay hop: numForwardingSubscribers() must be correct
  // when addChannelSubscriber runs on publisherExec, so forward flag is right from the start.
  auto sub = localFwd->addSubscriber(session, subReq, std::move(consumer));
  if (!sub) {
    co_return folly::makeUnexpected(makeAddSubscriberError(subReq.requestID));
  }
  bool forward = (localFwd->numForwardingSubscribers() > 0);

  // Relay phase: register + wire (+ maybe first-subscriber upstream subscribe) on relayExec_,
  // returning what the tail needs instead of mutating across the suspend.
  auto attach = co_await folly::coro::co_withExecutor(
      folly::getKeepAliveToken(relayExec_),
      attachNewLocalForwarderOnRelayExec(
          subReq,
          localReg,
          localFwd,
          subscriberExec,
          crossExecFilter,
          forward
      )
  );

  // Back on subscriberExec.

  if (pendingCb->sawOnEmpty_) {
    // All subscribers left during setup (localReg entry already removed by
    // PendingForwarderCallback::onEmpty). Drop the channel sub(s); the registry entry +
    // upstream sub remain, so a later subscribe takes the SubsequentSubscriber path.
    teardownLocalForwarderOnFailure(
        attach.publisherRef,
        subscriberExec,
        attach.ownsRelayChain ? relayExec_ : nullptr
    );
    co_return folly::makeUnexpected(SubscribeError{
        subReq.requestID,
        SubscribeErrorCode::INTERNAL_ERROR,
        "all subscribers cancelled during setup"
    });
  }

  if (attach.error) {
    teardownLocalForwarderOnFailure(
        attach.publisherRef,
        subscriberExec,
        attach.ownsRelayChain ? relayExec_ : nullptr,
        localFwd,
        attach.error->error().reasonPhrase
    );
    co_return std::move(*attach.error);
  }

  claim.setInitialState(attach.initial);
  // Also on the subscriber, so a post-SUBSCRIBE_OK joining fetch resolves.
  attach.initial.applyTo(*sub);
  replayPendingFowarderEvents(localFwd.get(), attach.finalCallback, *pendingCb, forward);
  localFwd->tryProcessNewGroupRequest(subReq.params);
  claim.markReady();
  co_return sub;
}

// === End multi-iothread subscribe helpers ===

folly::coro::Task<Publisher::SubscribeResult>
MoqxRelay::subscribe(SubscribeRequest subReq, std::shared_ptr<TrackConsumer> consumer) {
  return subscribeImpl(std::move(subReq), std::move(consumer));
}

folly::coro::Task<Publisher::SubscribeResult>
MoqxRelay::subscribeImpl(SubscribeRequest subReq, std::shared_ptr<TrackConsumer> consumer) {
  auto session = MoQSession::getRequestSession();
  maybeSetSessionExec(*session);
  const auto& ftn = subReq.fullTrackName;

  if (ftn.trackNamespace.empty()) {
    co_return folly::makeUnexpected(
        SubscribeError({subReq.requestID, SubscribeErrorCode::DOES_NOT_EXIST, "namespace required"})
    );
  }

  // TOCTOU fix: if we might be the first subscriber, wait for the upstream
  // connection before branching. A concurrent coroutine may emplace the entry
  // while we are suspended, so we re-check inside getOrCreateFromSubscribe.
  if (!registry_.exists(ftn) && upstream_ && !findUpstreamPublisher(ftn.trackNamespace)) {
    co_await upstream_->waitForConnected(kUpstreamConnectWaitTimeout);
  }

  consumer =
      wrapWithTrackStats(trackStats_, ftn, std::move(consumer), stats::TrackDirection::Egress);

  // upstreamSession is set by the factory for the first subscriber that needs to go upstream.
  std::shared_ptr<MoQSession> upstreamSession;
  auto firstOrSubsequent = registry_.getOrCreateFromSubscribe(
      ftn,
      shared_from_this(),
      [this, &ftn](std::shared_ptr<MoQForwarder> f) { return buildFilterChain(ftn, std::move(f)); },
      [this, &ftn, &upstreamSession](const std::shared_ptr<MoQForwarder>& f
      ) -> std::optional<ForwarderRef> {
        upstreamSession = namespaceTree_.findPublisherSession(ftn.trackNamespace);
        if (!upstreamSession) {
          return std::nullopt;
        }
        return makeForwarderRef(f, upstreamSession->getExecutor());
      }
  );

  if (std::get_if<SubscriptionRegistry::NoPublisher>(&firstOrSubsequent)) {
    co_return folly::makeUnexpected(SubscribeError(
        {subReq.requestID, SubscribeErrorCode::DOES_NOT_EXIST, "no such namespace or track"}
    ));
  }

  if (auto* first = std::get_if<SubscriptionRegistry::FirstSubscriber>(&firstOrSubsequent)) {
    auto upstreamPublisher = maybeWrapPublisher(relayExec_, upstreamSession);

    // Add subscriber first (with the client's original request) in case objects
    // arrive before subscribe OK.
    auto subscriber =
        first->forwarder->addSubscriber(std::move(session), subReq, std::move(consumer));
    if (!subscriber) {
      XLOG(ERR) << "addSubscriber returned null (draining?) for " << ftn
                << " reqID=" << subReq.requestID;
      co_return folly::makeUnexpected(makeAddSubscriberError(subReq.requestID));
    }
    XLOG(DBG4) << "added subscriber for ftn=" << ftn;

    // Subscribe upstream with forward set only while we have forwarding
    // subscribers, so an idle relay doesn't pull data it won't deliver.
    const auto clientRequestID = subReq.requestID;
    subReq =
        makeUpstreamSubReq(std::move(subReq), first->forwarder->numForwardingSubscribers() > 0);

    // Upstream subscribe + apply OK to the forwarder (NGR recorded without firing).
    // pending destructor fires on the error path.
    auto okOrErr = co_await subscribeUpstreamAndApplyOk(
        upstreamPublisher,
        std::move(subReq),
        first->consumer,
        first->forwarder,
        clientRequestID
    );
    if (okOrErr.hasError()) {
      co_return folly::makeUnexpected(std::move(okOrErr.error()));
    }
    auto& ok = okOrErr.value();
    InitialTrackState{ok.largest, ok.extensions}.applyTo(*subscriber);
    if (auto err = completeUpstreamSubscription(
            ftn,
            ok,
            first->pending,
            upstreamSession,
            upstreamPublisher,
            clientRequestID
        )) {
      co_return folly::makeUnexpected(std::move(*err));
    }
    co_return subscriber;

  } else {
    auto sub = co_await std::get<folly::coro::Task<SubscriptionRegistry::SubsequentSubscriber>>(
        std::move(firstOrSubsequent)
    );
    // LocalPublishFilter routes LF-mode subscribes to subscribeFromSubscriberExec, so
    // the entry's ref is owned here.
    auto forwarder = sub.forwarder.getIfOwned();
    XCHECK(forwarder) << "subscribeImpl reached with a remote forwarder ref";
    if (auto err = checkRangeNotInPast(*forwarder, subReq)) {
      co_return folly::makeUnexpected(std::move(*err));
    }
    co_return attachSubscriber(*forwarder, std::move(session), subReq, std::move(consumer));
  }
}

folly::coro::Task<Publisher::FetchResult>
MoqxRelay::fetch(Fetch fetch, std::shared_ptr<FetchConsumer> consumer) {
  return fetchImpl(std::move(fetch), std::move(consumer));
}

namespace {
TrackStatusOk buildTrackStatusOk(MoQForwarder& fwd, bool hasHandle, const TrackStatus& req) {
  TrackStatusCode statusCode = TrackStatusCode::TRACK_NOT_STARTED;
  // largest() set means an object arrived; hasHandle means the upstream sub is still live.
  if (fwd.largest()) {
    statusCode = hasHandle ? TrackStatusCode::IN_PROGRESS : TrackStatusCode::UNKNOWN;
  }
  TrackStatusOk ok;
  ok.requestID = req.requestID;
  ok.groupOrder = fwd.groupOrder();
  ok.largest = fwd.largest();
  ok.fullTrackName = req.fullTrackName;
  ok.statusCode = statusCode;
  return ok;
}
} // namespace

std::optional<Publisher::TrackStatusResult>
MoqxRelay::trackStatusOnSubscriberExec(const TrackStatus& req) {
  if (req.fullTrackName.trackNamespace.empty()) {
    return std::nullopt;
  }
  auto* localReg = tlForwarders_.get();
  auto localFwd = localReg ? localReg->getIfReady(req.fullTrackName) : nullptr;
  if (!localFwd || localFwd->numForwardingSubscribers() == 0) {
    return std::nullopt;
  }
  // A forwarding local subscriber implies the upstream sub is live.
  return Publisher::TrackStatusResult(buildTrackStatusOk(*localFwd, /*hasHandle=*/true, req));
}

Fetch MoqxRelay::fetchOnSubscriberExec(Fetch fetch, const std::shared_ptr<MoQSession>& session) {
  auto [standalone, joining] = fetchType(fetch);
  if (!joining) {
    return fetch;
  }
  auto* localReg = tlForwarders_.get();
  auto localFwd = localReg ? localReg->getIfReady(fetch.fullTrackName) : nullptr;
  if (localFwd) {
    auto res = localFwd->resolveJoiningFetch(session, *joining);
    if (res.hasValue()) {
      fetch.args = StandaloneFetch(res.value().start, res.value().end);
      return fetch;
    }
  }
  // Not resolvable yet (no largest, or pre-PUBLISH_OK on draft-18's separate
  // stream): defer upstream by track name. Rare; fails if there's no upstream.
  joining->joiningRequestID = std::nullopt;
  return fetch;
}

folly::coro::Task<Publisher::FetchResult>
MoqxRelay::fetchImpl(Fetch fetch, std::shared_ptr<FetchConsumer> consumer) {
  auto session = MoQSession::getRequestSession();

  if (fetch.fullTrackName.trackNamespace.empty()) {
    co_return folly::makeUnexpected(
        FetchError({fetch.requestID, FetchErrorCode::DOES_NOT_EXIST, "namespace required"})
    );
  }

  auto [standalone, joining] = fetchType(fetch);
  // LF mode resolves/defers joining in fetchOnSubscriberExec on the subscriber exec.
  if (joining && mode() != Mode::LocalForwarder) {
    auto fetchView = registry_.getFetchView(fetch.fullTrackName);
    if (!fetchView) {
      XLOG(ERR) << "No subscription for joining fetch";
      co_return folly::makeUnexpected(FetchError(
          {fetch.requestID, FetchErrorCode::DOES_NOT_EXIST, "No subscription for joining fetch"}
      ));
    } else if (fetchView->isReady) {
      // Non-LF only (guarded above): the entry's ref is owned here.
      auto forwarder = fetchView->forwarder.getIfOwned();
      XCHECK(forwarder) << "joining fetch against a remote-owned entry";
      auto res = forwarder->resolveJoiningFetch(session, *joining);
      if (res.hasError()) {
        co_return folly::makeUnexpected(res.error());
      }
      fetch.args = StandaloneFetch(res.value().start, res.value().end);
      joining = nullptr;
    } else {
      // Upstream is resolving the subscribe; let MoQSession resolve the
      // request ID by track name to avoid a cross-executor data race.
      joining->joiningRequestID = std::nullopt;
    }
  }

  auto upstreamPublisher = findUpstreamPublisher(fetch.fullTrackName.trackNamespace);
  if (!upstreamPublisher && upstream_) {
    co_await upstream_->waitForConnected(kUpstreamConnectWaitTimeout);
    upstreamPublisher = findUpstreamPublisher(fetch.fullTrackName.trackNamespace);
  }
  if (!upstreamPublisher) {
    // Attempt to find matching upstream subscription (from publish)
    if (auto fetchView = registry_.getFetchView(fetch.fullTrackName)) {
      upstreamPublisher = fetchView->publisher;
    }
    if (!upstreamPublisher) {
      co_return folly::makeUnexpected(
          FetchError({fetch.requestID, FetchErrorCode::DOES_NOT_EXIST, "no upstream for fetch"})
      );
    }
  }
  fetch.priority = kDefaultUpstreamPriority;

  if (!cache_ || joining) {
    // We can't use the cache on an unresolved joining fetch - we don't know
    // which objects are being requested.  However, once we have that resolved,
    // we SHOULD be able to serve from cache.
    if (standalone) {
      XLOG(DBG1) << "Upstream fetch {" << standalone->start.group << "," << standalone->start.object
                 << "}.." << standalone->end.group << "," << standalone->end.object << "}";
    }
    co_return co_await upstreamPublisher->fetch(std::move(fetch), std::move(consumer));
  }
  co_return co_await cache_
      ->fetch(std::move(fetch), std::move(consumer), std::move(upstreamPublisher));
}

folly::coro::Task<std::optional<TrackStatusOk>>
MoqxRelay::readPublisherForwarderStatus(bool hasHandle, TrackStatus req) {
  auto* localReg = tlForwarders_.get();
  auto forwarder = localReg ? localReg->getIfReady(req.fullTrackName) : nullptr;
  if (!forwarder || forwarder->numForwardingSubscribers() == 0) {
    co_return std::nullopt;
  }
  co_return buildTrackStatusOk(*forwarder, hasHandle, req);
}

folly::coro::Task<Publisher::TrackStatusResult> MoqxRelay::trackStatus(TrackStatus trackStatus) {
  return trackStatusImpl(std::move(trackStatus));
}

folly::coro::Task<Publisher::TrackStatusResult> MoqxRelay::trackStatusImpl(TrackStatus trackStatus
) {
  XLOG(DBG1) << __func__ << " ftn=" << trackStatus.fullTrackName;

  auto session = MoQSession::getRequestSession();

  if (trackStatus.fullTrackName.trackNamespace.empty()) {
    co_return folly::makeUnexpected(TrackStatusError(
        {trackStatus.requestID, TrackStatusErrorCode::DOES_NOT_EXIST, "namespace required"}
    ));
  }

  auto upstreamView = registry_.getUpstreamView(trackStatus.fullTrackName);
  // Active subscription: answer from the publisher forwarder's state instead of going upstream.
  std::optional<TrackStatusOk> trackStatusOk;
  if (mode() == Mode::LocalForwarder) {
    // LF mode never touches registry->forwarder; read it via tlForwarders_ on the publisher exec.
    if (upstreamView && upstreamView->publisherExec) {
      trackStatusOk = co_await folly::coro::co_withExecutor(
          folly::getKeepAliveToken(upstreamView->publisherExec),
          readPublisherForwarderStatus((bool)upstreamView->handle, trackStatus)
      );
    }
  } else if (upstreamView) {
    // Non-LF: relayExec_ (or the single thread) owns the sole forwarder; read it inline.
    auto forwarder = upstreamView->forwarder.getIfOwned();
    if (forwarder && forwarder->numForwardingSubscribers() > 0) {
      trackStatusOk = buildTrackStatusOk(*forwarder, (bool)upstreamView->handle, trackStatus);
    }
  }
  if (trackStatusOk) {
    XLOG(DBG1) << "Returning local track status for " << trackStatus.fullTrackName
               << " statusCode=" << (uint32_t)trackStatusOk->statusCode;
    co_return std::move(*trackStatusOk);
  }
  // No active subscription — fall through to the upstream path.
  {
    // No active subscription — try registry publisher first, then namespace tree
    std::shared_ptr<Publisher> upstreamPublisher;
    if (upstreamView) {
      upstreamPublisher = upstreamView->publisher;
    } else {
      upstreamPublisher = findUpstreamPublisher(trackStatus.fullTrackName.trackNamespace);
      if (!upstreamPublisher && upstream_) {
        co_await upstream_->waitForConnected(kUpstreamConnectWaitTimeout);
        upstreamPublisher = findUpstreamPublisher(trackStatus.fullTrackName.trackNamespace);
      }
    }
    if (!upstreamPublisher) {
      XLOG(DBG1) << "No upstream for track: " << trackStatus.fullTrackName;
      co_return folly::makeUnexpected(TrackStatusError{
          trackStatus.requestID,
          TrackStatusErrorCode::DOES_NOT_EXIST,
          "no such namespace or track"
      });
    }
    auto result = co_await upstreamPublisher->trackStatus(std::move(trackStatus));

    if (result.hasError()) {
      XLOG(DBG1) << "Upstream trackStatus failed: " << result.error().reasonPhrase;
    } else {
      XLOG(DBG1) << "Upstream trackStatus succeeded";
    }
    co_return result;
  }
}

void MoqxRelay::onEmpty(MoQForwarder* forwarder) {
  onEmptyImpl(forwarder->fullTrackName());
}

void MoqxRelay::onEmptyImpl(const FullTrackName& ftn) {
  auto upstreamView = registry_.getUpstreamView(ftn);
  if (!upstreamView) {
    return;
  }

  if (!upstreamView->handle) {
    // Handle is null - publisher terminated via FilterConsumer
    XLOG(INFO) << "Publisher terminated for " << ftn;
    registry_.remove(ftn);
    return;
  }

  // Handle exists - just last subscriber left. requestUpdate/unsubscribe mutate
  // the upstream session inline (no self-hop), so they must run on publisherExec.
  XLOG(INFO) << "Last subscriber removed for " << ftn;
  XCHECK(upstreamView->publisherExec);
  if (upstreamView->isPublish) {
    // if it's publish, don't unsubscribe, just subscribeUpdate forward=false
    XLOG(DBG1) << "Updating upstream subscription forward=false";
    launchUpdate(
        upstreamView->publisherExec,
        doSubscribeUpdate(upstreamView->handle, /*forward=*/false)
    );
  } else {
    runOnSessionExec(relayExec_, upstreamView->publisherExec, [h = upstreamView->handle] {
      h->unsubscribe();
    });
    XLOG(DBG4) << "Erasing subscription to " << ftn;
    registry_.remove(ftn);
  }
}

void MoqxRelay::forwardChanged(MoQForwarder* forwarder, bool forward) {
  forwardChangedImpl(forwarder->fullTrackName(), forward);
}

void MoqxRelay::forwardChangedImpl(const FullTrackName& ftn, bool forward) {
  auto upstreamView = registry_.getUpstreamView(ftn);
  if (!upstreamView) {
    return;
  }
  if (!upstreamView->isReady) {
    // Ignore: it's the first subscriber, forward update not needed
    return;
  }
  if (!upstreamView->handle) {
    // Publisher terminated (onPublishDone cleared handle/upstream)
    XLOG(DBG4) << "Ignoring forward change for " << ftn << " - publisher terminated";
    return;
  }
  XLOG(INFO) << "Updating forward for " << ftn << " forward=" << forward;

  // handle non-null (checked above) implies upstream is live, so publisherExec is set.
  XCHECK(upstreamView->publisherExec);
  launchUpdate(upstreamView->publisherExec, doSubscribeUpdate(upstreamView->handle, forward));
}

void MoqxRelay::newGroupRequested(MoQForwarder* forwarder, uint64_t group) {
  newGroupRequestedImpl(forwarder->fullTrackName(), group);
}

void MoqxRelay::newGroupRequestedImpl(const FullTrackName& ftn, uint64_t group) {
  auto upstreamView = registry_.getUpstreamView(ftn);
  // Check if handle is still valid (publisher may have terminated)
  if (!upstreamView || !upstreamView->handle) {
    XLOG(DBG4) << "Ignoring NEW_GROUP_REQUEST for " << ftn << " - publisher terminated";
    return;
  }
  XLOG(INFO) << "New group request detected for " << ftn;

  // handle non-null (checked above) implies upstream is live, so publisherExec is set.
  XCHECK(upstreamView->publisherExec);
  launchUpdate(upstreamView->publisherExec, doNewGroupRequestUpdate(upstreamView->handle, group));
}

// TRACK_FILTER support

std::shared_ptr<PropertyRanking> MoqxRelay::getOrCreateRanking(
    std::shared_ptr<NamespaceTree::NamespaceNode> node,
    uint64_t propertyType,
    const TrackNamespace& ns
) {
  auto& ranking = namespaceTree_.getOrInsertRanking(*node, propertyType);
  if (!ranking) {
    ranking = std::make_shared<PropertyRanking>(
        propertyType,
        maxDeselected_,
        idleTimeout_,
        std::chrono::milliseconds(0), // sweepThrottle wired in subsequent commit
        [this](const FullTrackName& ftn) -> std::chrono::steady_clock::time_point {
          auto view = registry_.getTopNView(ftn);
          return view ? view->lastObjectTime : std::chrono::steady_clock::time_point{};
        },
        // Batch callback: called once per track-selected event with all sessions
        [this](
            const FullTrackName& ftn,
            const std::vector<std::pair<std::shared_ptr<MoQSession>, bool>>& sessions
        ) {
          for (const auto& [session, forward] : sessions) {
            onTrackSelected(ftn, session, forward);
          }
        },
        // Individual callback: called by addSessionToTopNGroup to notify a newly
        // joined session of tracks already in top-N at the time it subscribes.
        [this](const FullTrackName& ftn, std::shared_ptr<MoQSession> session, bool forward) {
          onTrackSelected(ftn, session, forward);
        },
        // Eviction callback
        [this](const FullTrackName& ftn, std::shared_ptr<MoQSession> session) {
          onTrackEvicted(ftn, session);
        }
    );

    // Retroactively register tracks already published under this node and all
    // descendants. A subscriber at /conf should see tracks at /conf/room1/track1.
    namespaceTree_.forEachNodeInSubtree(
        ns,
        node,
        [&](const TrackNamespace& prefix, std::shared_ptr<NamespaceTree::NamespaceNode> current) {
          // Collect tracks at this level with their last-activity time and current
          // property value, then sort by lastObjectTime ascending so arrivalSeq
          // assignment matches what would have happened if the subscription arrived
          // before the publishers.
          struct RetroTrack {
            std::string trackName;
            std::shared_ptr<moxygen::MoQSession> publishSession;
            std::optional<uint64_t> initialPropertyValue;
            std::chrono::steady_clock::time_point lastObjectTime;
          };
          std::vector<RetroTrack> retroTracks;
          retroTracks.reserve(current->publishCount());

          current->forEachPublish([&](const std::string& trackName,
                                      const std::shared_ptr<MoQSession>& publishSession) {
            FullTrackName ftn{prefix, trackName};
            std::optional<uint64_t> initialPropertyValue;
            std::chrono::steady_clock::time_point lastObjectTime{};
            auto topNView = registry_.getTopNView(ftn);
            if (topNView) {
              lastObjectTime = topNView->lastObjectTime;
              if (auto forwarder = topNView->forwarder.getIfOwned()) {
                initialPropertyValue = forwarder->extensions().getIntExtension(propertyType);
              }
              // else: (LF) the forwarder's extensions are not readable here, the ranking is built
              // as new objects arrive.
              if (topNView->topNFilter) {
                // Wire value-change, track-ended, and activity observers to the existing
                // TopNFilter.
                auto rankingPtr = ranking;
                topNView->topNFilter->registerObserver(
                    propertyType,
                    PropertyObserver{
                        .onValueChanged = [rankingPtr, ftn](uint64_t value
                                          ) { rankingPtr->updateSortValue(ftn, value); },
                        .onTrackEnded = [rankingPtr, ftn]() { rankingPtr->removeTrack(ftn); },
                        .onActivity = [rankingPtr]() { rankingPtr->sweepIdle(); }
                    }
                );
              }
            }
            retroTracks.push_back({trackName, publishSession, initialPropertyValue, lastObjectTime}
            );
          });

          std::sort(
              retroTracks.begin(),
              retroTracks.end(),
              [](const RetroTrack& a, const RetroTrack& b) {
                return a.lastObjectTime < b.lastObjectTime;
              }
          );

          for (const auto& t : retroTracks) {
            FullTrackName ftn{prefix, t.trackName};
            ranking->registerTrack(ftn, t.initialPropertyValue, t.publishSession);
            XLOG(DBG4) << "[getOrCreateRanking] Retroactively registered track " << ftn;
          }
        }
    );
  }
  return ranking;
}

void MoqxRelay::onTrackSelected(
    const FullTrackName& ftn,
    std::shared_ptr<MoQSession> session,
    bool forward
) {
  XLOG(DBG4) << "[MoqxRelay] Track selected: " << ftn << " session=" << session.get()
             << " forward=" << forward;

  if (!session) {
    XLOG(ERR) << "onTrackSelected: null session for " << ftn;
    return;
  }

  auto trackForwarder = registry_.getForwarderRef(ftn);
  if (!trackForwarder) {
    XLOG(DBG4) << "onTrackSelected: no subscription for " << ftn;
    return;
  }

  // TRACK_FILTER subscribers are unpinned so onTrackEvicted can remove them.
  addSubscriberAndPublish(session, trackForwarder, forward, /*pinned=*/false);
}

void MoqxRelay::onTrackEvicted(const FullTrackName& ftn, std::shared_ptr<MoQSession> session) {
  XLOG(DBG4) << "[MoqxRelay] Track evicted: " << ftn << " session=" << session.get();

  if (!session) {
    XLOG(WARN) << "onTrackEvicted: null session for " << ftn;
    return;
  }

  // PublishDone makes removeSubscriber notify the downstream via publishDone() rather
  // than silently dropping it.
  auto evict = [session](const std::shared_ptr<MoQForwarder>& fwd) {
    if (!fwd) {
      return;
    }
    auto sub = fwd->getSubscriber(session.get());
    if (!sub || sub->isPinned()) {
      XLOG(DBG4) << "onTrackEvicted: pinned/missing subscriber, skipping";
      return;
    }
    fwd->removeSubscriber(
        session,
        PublishDone{RequestID(0), PublishDoneStatusCode::SUBSCRIPTION_ENDED, 0, "evicted"},
        "onTrackEvicted"
    );
  };

  if (mode() == Mode::LocalForwarder) {
    // The subscriber lives on the per-thread local forwarder, not the registry's
    // publisher forwarder; evict it on its owning exec.
    folly::via(session->getExecutor(), [this, ftn, evict = std::move(evict)]() {
      auto* localReg = tlForwarders_.get();
      evict(localReg ? localReg->getForEviction(ftn) : nullptr);
    });
  } else {
    evict(registry_.getForwarderRef(ftn).getIfOwned());
  }
}

MoqxRelay::TrackMatch
MoqxRelay::matchTracks(const TrackNamespace& nsPrefix, const std::string* trackName, size_t limit)
    const {
  TrackMatch match;
  registry_.forEachName([&](const FullTrackName& ftn) {
    if (!ftn.trackNamespace.startsWith(nsPrefix)) {
      return;
    }
    if (trackName && ftn.trackName != *trackName) {
      return;
    }
    ++match.matched;
    if (match.keys.size() < limit) {
      match.keys.push_back(ftn);
    }
  });
  return match;
}

void MoqxRelay::dumpState(RelayStateVisitor& visitor) const {
  visitor.onPeersBegin();
  for (const auto& [sess, peer] : peerSubNsHandles_) {
    visitor.onPeer(sess->getPeerAddress().describe(), sess->getAuthority(), peer.relayID);
  }
  visitor.onPeersEnd();
  if (!visitor.alive()) {
    return;
  }

  visitor.onSubscriptionsBegin();
  registry_.forEach([&](const SubscriptionRegistry::EntryView& e) {
    std::string sourceAddr;
    if (e.upstream) {
      sourceAddr = e.upstream->getPeerAddress().describe();
    }
    // Counted by RelayIngestFilter on this executor rather than read off the
    // forwarder, which in LocalForwarder mode is owned by another one. There
    // the counts are what the relay chain saw, downstream of the cross-exec
    // hop, so they can trail the forwarder's own by whatever is in flight.
    RelayStateVisitor::SubscriptionInfo info{
        .ftn = e.ftn,
        .isPublish = e.isPublish,
        .largest = e.ingest.largest,
        .totalGroupsReceived = e.ingest.groups,
        .totalObjectsReceived = e.ingest.objects,
        .sourceAddress = sourceAddr,
    };
    visitor.onSubscription(info);
  });
  visitor.onSubscriptionsEnd();
  if (!visitor.alive()) {
    return;
  }

  visitor.onNamespaceTreeBegin();
  namespaceTree_.walkTree(
      [&](std::string_view childKey, const NamespaceTree::NamespaceNode& node) {
        std::string publisherAddr;
        if (node.publisherSession()) {
          publisherAddr = node.publisherSession()->getPeerAddress().describe();
        }
        visitor.beginNamespaceNode(
            childKey,
            node.trackNamespace,
            node.subscriberCount(),
            publisherAddr,
            node.publisherPeerID()
        );
      },
      [&]() { visitor.endNamespaceNode(); }
  );
  visitor.onNamespaceTreeEnd();
  if (!visitor.alive()) {
    return;
  }

  if (cache_) {
    visitor.onCacheBegin(cache_->totalCachedBytes(), MoqxCache::SteadyClock::now());
    cache_->forEachTrackStats([&](const MoqxCache::TrackStatsView& track) {
      return visitor.onCacheTrack(track);
    });
    visitor.onCacheEnd();
  }
}

} // namespace openmoq::moqx
