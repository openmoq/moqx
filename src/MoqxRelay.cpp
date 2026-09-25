/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Originally from github.com/facebookexperimental/moxygen.
 * See the moxygen LICENSE for the original license terms:
 * https://github.com/openmoq/moxygen/blob/main/LICENSE
 *
 * Copyright (c) OpenMOQ contributors.
 */

#include "MoqxRelay.h"
#include "LocalForwarderRelay.h"
#include "relay/CrossExecSubscriptionHandle.h"
#include "relay/InitialTrackState.h"
#include "relay/PublisherCrossExecFilter.h"
#include "relay/RelayDetail.h"
#include "relay/SubscriberCrossExecFilter.h"
#include "relay/TrackStatsFilter.h"
#include "relay/WeakRelayForwarderCallback.h"
#include <algorithm>
#include <folly/Random.h>
#include <folly/container/F14Set.h>
#include <folly/coro/Collect.h>
#include <moxygen/MoQFilters.h>
#include <moxygen/MoQTrackProperties.h>

namespace {

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
} // namespace

using namespace moxygen;

namespace openmoq::moqx {

using namespace detail;

uint64_t generateRelayHopID() {
  uint64_t hopID = 0;
  do {
    folly::Random::secureRandom(&hopID, sizeof(hopID));
    hopID &= kMaxRelayHopID;
  } while (hopID == 0);
  return hopID;
}

MoqxRelay::MoqxRelay(
    config::CacheConfig cache,
    std::string relayID,
    uint64_t relayHopID,
    std::shared_ptr<folly::Executor> relayExec,
    uint64_t maxDeselected,
    std::chrono::milliseconds idleTimeout,
    std::chrono::milliseconds activityThreshold
)
    : relayID_(std::move(relayID)),
      relayHopID_(relayHopID == 0 ? generateRelayHopID() : relayHopID),
      ownedRelayExec_(std::move(relayExec)), relayExec_(ownedRelayExec_.get()),
      maxDeselected_(maxDeselected), idleTimeout_(idleTimeout),
      activityThreshold_(activityThreshold) {
  XCHECK_LE(relayHopID_, kMaxRelayHopID);
  // Park timers fire on the relay's own EventBase instead of folly's global
  // timekeeper thread. Null (SingleThread mode, or a non-folly exec) falls back to it.
  if (auto* follyExec = dynamic_cast<moxygen::MoQFollyExecutorImpl*>(relayExec_)) {
    timekeeper_ =
        std::make_unique<folly::EventBaseThreadTimekeeper>(*follyExec->getBackingEventBase());
  }
  if (cache.maxCachedTracks > 0) {
    cache_ = std::make_unique<MoqxCache>(cache.maxCachedTracks, cache.maxCachedGroupsPerTrack);
    cache_->setMaxCachedBytes(static_cast<size_t>(cache.maxCachedMb) * 1024 * 1024);
    cache_->setMinEvictionBytes(static_cast<size_t>(cache.minEvictionKb) * 1024);
    cache_->setDefaultMaxCacheDuration(cache.defaultMaxCacheDuration);
    cache_->setMaxAllowedCacheDuration(cache.maxCacheDuration);
  }
}

std::shared_ptr<MoqxRelay> MoqxRelay::create(
    config::CacheConfig cache,
    std::string relayID,
    uint64_t relayHopID,
    std::shared_ptr<folly::Executor> relayExec,
    bool useLocalForwarders,
    uint64_t maxDeselected,
    std::chrono::milliseconds idleTimeout,
    std::chrono::milliseconds activityThreshold
) {
  if (useLocalForwarders) {
    return std::make_shared<LocalForwarderRelay>(
        std::move(cache),
        std::move(relayID),
        relayHopID,
        std::move(relayExec),
        maxDeselected,
        idleTimeout,
        activityThreshold
    );
  }
  struct OwnedRelay : MoqxRelay {
    OwnedRelay(
        config::CacheConfig cache,
        std::string relayID,
        uint64_t relayHopID,
        std::shared_ptr<folly::Executor> relayExec,
        uint64_t maxDeselected,
        std::chrono::milliseconds idleTimeout,
        std::chrono::milliseconds activityThreshold
    )
        : MoqxRelay(
              std::move(cache),
              std::move(relayID),
              relayHopID,
              std::move(relayExec),
              maxDeselected,
              idleTimeout,
              activityThreshold
          ) {}
  };
  return std::make_shared<OwnedRelay>(
      std::move(cache),
      std::move(relayID),
      relayHopID,
      std::move(relayExec),
      maxDeselected,
      idleTimeout,
      activityThreshold
  );
}

std::shared_ptr<moxygen::Publisher> MoqxRelay::createPublisherFilter() {
  if (relayExec_) {
    return std::make_shared<PublisherCrossExecFilter>(relayExec_, shared_from_this());
  }
  return shared_from_this();
}

std::shared_ptr<moxygen::Subscriber> MoqxRelay::createSubscriberFilter() {
  if (relayExec_) {
    return std::make_shared<SubscriberCrossExecFilter>(relayExec_, shared_from_this());
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
  pendingRendezvous_.wakeUnderNamespace(pubNs.trackNamespace);
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

  releasePublisherEntry(ftn);
}

void MoqxRelay::releasePublisherEntry(const FullTrackName& ftn) {
  // Clears handle + upstream; erases if no subscribers remain.
  auto kept = registry_.onPublisherTerminated(ftn);
  if (!kept) {
    XLOG(DBG1) << "Publisher terminated with no subscribers, cleaning up " << ftn;
  }
}

// Validates a publish namespace against allowedNamespacePrefix_ (UNINTERESTED)
// and non-emptiness (INTERNAL_ERROR, unless the negotiated draft allows it).
// Returns std::nullopt on success. Safe to call on any thread (reads only
// immutable config plus the caller-supplied emptyNamespaceAllowed).
std::optional<PublishError> MoqxRelay::validatePublishNamespace(
    const FullTrackName& ftn,
    RequestID requestID,
    bool emptyNamespaceAllowed
) const {
  if (!ftn.trackNamespace.startsWith(allowedNamespacePrefix_)) {
    return PublishError{requestID, PublishErrorCode::UNINTERESTED, "bad namespace"};
  }
  if (ftn.trackNamespace.empty() && !emptyNamespaceAllowed) {
    return PublishError{requestID, PublishErrorCode::INTERNAL_ERROR, "namespace required"};
  }
  return std::nullopt;
}

// Draft 18+ allows an empty track namespace (PUBLISH/SUBSCRIBE/FETCH/TRACK_STATUS).
bool MoqxRelay::emptyNamespaceAllowed(const std::shared_ptr<MoQSession>& session) {
  auto maybeNegotiatedVersion = session->getNegotiatedVersion();
  XCHECK(maybeNegotiatedVersion.has_value());
  return getDraftMajorVersion(*maybeNegotiatedVersion) >= 18;
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
  if (auto err = validatePublishNamespace(
          pub.fullTrackName,
          pub.requestID,
          emptyNamespaceAllowed(session)
      )) {
    return folly::makeUnexpected(std::move(*err));
  }

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
  // Strong ref to forwarder is allowed in non-LF modes
  auto chainForwarder = publisherRef.getIfOwned();

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

  wireForwarderCallback(chainForwarder);

  uint64_t nForwardingSubscribers = 0;
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
      nForwardingSubscribers += info.forward ? 1 : 0;
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
      nForwardingSubscribers += info.forward ? 1 : 0;
      if (!addSubscriberAndPublish(outSession, publisherRef, info.forward, /*pinned=*/true)) {
        XLOG(ERR) << "addSubscriberAndPublish failed for " << pub.fullTrackName;
        continue;
      }
    }
  }

  // Forward if a direct subscriber is forwarding, or for any TRACK_FILTER subscriber
  // (PropertyRanking needs objects to evaluate property values for ranking).
  // When subscribers join later via subscribeNamespace, forwardChanged() sends REQUEST_UPDATE.
  bool shouldForward = (nForwardingSubscribers > 0) || hasTrackFilterSub;

  // Wake any SUBSCRIBE that's parked waiting for this exact track (draft 18+
  // RENDEZVOUS_TIMEOUT).
  pendingRendezvous_.wakeForTrack(pub.fullTrackName);

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

void MoqxRelay::wireForwarderCallback(const std::shared_ptr<MoQForwarder>& chainForwarder) {
  // Weak ref breaks the registry → forwarder → callback → relay cycle.
  XCHECK(chainForwarder) << "publishWithSession: null chainForwarder in non-LF mode";
  chainForwarder->setCallback(std::make_shared<WeakRelayForwarderCallback>(weak_from_this()));
}

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
  auto peerHandle = makePeerHandle(subscriber);
  Subscriber::PublishResult pub;
  if (subscriberExec) {
    SubscriberCrossExecFilter wrapped(subscriberExec, session);
    pub = wrapped.publish(subscriber->getPublishRequest(), std::move(peerHandle));
  } else {
    pub = session->publish(subscriber->getPublishRequest(), std::move(peerHandle));
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

std::shared_ptr<Publisher::SubscriptionHandle>
MoqxRelay::makePeerHandle(std::shared_ptr<MoQForwarder::Subscriber> subscriber) {
  // relayExec_ owns the forwarder, but the session calls unsubscribe() on its own io
  // thread, so the handle has to hop before it reaches subscribers_.
  if (relayExec_) {
    return std::make_shared<CrossExecSubscriptionHandle>(std::move(subscriber), relayExec_);
  }
  return subscriber;
}

// Calls startPublish sync and fires the reply async. Returns false on synchronous failure.
bool MoqxRelay::addSubscriberAndPublish(
    std::shared_ptr<MoQSession> subscriberSession,
    const ForwarderRef& publisherRef,
    bool forward,
    bool pinned
) {
  XCHECK(publisherRef) << "addSubscriberAndPublish: empty forwarder ref";
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

ForwarderRef MoqxRelay::makeForwarderRef(
    const std::shared_ptr<MoQForwarder>& forwarder,
    folly::Executor* /*publisherExec*/
) const {
  return ForwarderRef::owned(forwarder);
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

MoqxRelay::IngestChain
MoqxRelay::makeIngestChain(const FullTrackName& ftn, std::shared_ptr<TrackConsumer> downstream) {
  auto ingest = std::make_shared<IngestCounters>();
  auto ingestFilter =
      std::make_shared<RelayIngestFilter>(shared_from_this(), ftn, ingest, std::move(downstream));
  auto topNFilter =
      std::make_shared<TopNFilter>(ftn, std::static_pointer_cast<TrackConsumer>(ingestFilter));
  topNFilter->setActivityThreshold(activityThreshold_);
  auto chainHead = wrapWithTrackStats(
      trackStats_,
      ftn,
      std::static_pointer_cast<TrackConsumer>(topNFilter),
      stats::TrackDirection::Ingest
  );
  return IngestChain{std::move(ingest), std::move(topNFilter), std::move(chainHead)};
}

SubscriptionRegistry::FilterChainResult
MoqxRelay::buildFilterChain(const FullTrackName& ftn, std::shared_ptr<MoQForwarder> forwarder) {
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
  auto chain = makeIngestChain(ftn, std::static_pointer_cast<TrackConsumer>(forwarder));
  return SubscriptionRegistry::FilterChainResult{
      .consumer = chain.chainHead,
      .topNFilter = std::move(chain.topNFilter),
      .chainHead = chain.chainHead,
      .ingest = std::move(chain.ingest)
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
  auto pubNode =
      namespaceTree_.findNode(subTracks.trackNamespacePrefix, /*createMissingNodes=*/false);
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
  auto nodePtr = namespaceTree_.findNode(ftn.trackNamespace, /*createMissingNodes=*/false);

  if (!nodePtr) {
    return state;
  }

  state.nodeExists = true;

  state.session = nodePtr->findPublishSession(ftn.trackName);

  return state;
}

// === Multi-iothread subscribe helpers ===

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

  if (ftn.trackNamespace.empty() && !emptyNamespaceAllowed(session)) {
    co_return folly::makeUnexpected(
        SubscribeError({subReq.requestID, SubscribeErrorCode::DOES_NOT_EXIST, "namespace required"})
    );
  }

  // Resolve (or wait out our own rendezvous) before ever committing a
  // FirstSubscriber/SubsequentSubscriber entry, so a concurrent SUBSCRIBE for the
  // same ftn is never bound to our RENDEZVOUS_TIMEOUT.
  if (auto rendezvousTimeout = takeRendezvousTimeout(subReq, session)) {
    if (auto rendezvousErr =
            co_await rendezvousWithPublisherOrTimeout(subReq, *rendezvousTimeout)) {
      co_return folly::makeUnexpected(std::move(*rendezvousErr));
    }
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
    subReq = makeUpstreamSubReq(
        std::move(subReq),
        first->forwarder->numForwardingSubscribers() > 0,
        upstreamSession
    );

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

folly::coro::Task<Publisher::FetchResult>
MoqxRelay::fetchImpl(Fetch fetch, std::shared_ptr<FetchConsumer> consumer) {
  auto session = MoQSession::getRequestSession();

  if (fetch.fullTrackName.trackNamespace.empty() && !emptyNamespaceAllowed(session)) {
    co_return folly::makeUnexpected(
        FetchError({fetch.requestID, FetchErrorCode::DOES_NOT_EXIST, "namespace required"})
    );
  }

  auto [standalone, joining] = fetchType(fetch);
  if (joining) {
    if (auto err = resolveJoiningFetchOnRelay(fetch, joining, session)) {
      co_return folly::makeUnexpected(std::move(*err));
    }
  }

  // Prefer an exact-track upstream (from publish/subscribe) over a broader
  // namespace-level publisher, matching subscribeImpl's resolution order.
  auto fetchView = registry_.getFetchView(fetch.fullTrackName);
  std::shared_ptr<Publisher> upstreamPublisher;
  if (fetchView) {
    upstreamPublisher = fetchView->publisher;
  } else {
    upstreamPublisher = findUpstreamPublisher(fetch.fullTrackName.trackNamespace);
    if (!upstreamPublisher && upstream_) {
      co_await upstream_->waitForConnected(kUpstreamConnectWaitTimeout);
      upstreamPublisher = findUpstreamPublisher(fetch.fullTrackName.trackNamespace);
    }
  }
  if (!upstreamPublisher) {
    co_return folly::makeUnexpected(
        FetchError({fetch.requestID, FetchErrorCode::DOES_NOT_EXIST, "no upstream for fetch"})
    );
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

std::optional<FetchError> MoqxRelay::resolveJoiningFetchOnRelay(
    Fetch& fetch,
    JoiningFetch*& joining,
    const std::shared_ptr<MoQSession>& session
) {
  auto fetchView = registry_.getFetchView(fetch.fullTrackName);
  if (!fetchView) {
    XLOG(ERR) << "No subscription for joining fetch";
    return FetchError(
        {fetch.requestID, FetchErrorCode::DOES_NOT_EXIST, "No subscription for joining fetch"}
    );
  } else if (fetchView->isReady) {
    // The entry's ref is owned here.
    auto forwarder = fetchView->forwarder.getIfOwned();
    XCHECK(forwarder) << "joining fetch against a remote-owned entry";
    auto res = forwarder->resolveJoiningFetch(session, *joining);
    if (res.hasError()) {
      return res.error();
    }
    fetch.args = StandaloneFetch(res.value().start, res.value().end);
    joining = nullptr;
  } else {
    // Upstream is resolving the subscribe; let MoQSession resolve the
    // request ID by track name to avoid a cross-executor data race.
    joining->joiningRequestID = std::nullopt;
  }
  return std::nullopt;
}

folly::coro::Task<std::optional<TrackStatusOk>> MoqxRelay::readLocalTrackStatus(
    const SubscriptionRegistry::UpstreamView& upstreamView,
    const TrackStatus& req
) {
  // Non-LF: relayExec_ (or the single thread) owns the sole forwarder; read it inline.
  auto forwarder = upstreamView.forwarder.getIfOwned();
  if (forwarder && forwarder->numForwardingSubscribers() > 0) {
    co_return buildTrackStatusOk(*forwarder, (bool)upstreamView.handle, req);
  }
  co_return std::nullopt;
}

folly::coro::Task<Publisher::TrackStatusResult> MoqxRelay::trackStatus(TrackStatus trackStatus) {
  return trackStatusImpl(std::move(trackStatus));
}

folly::coro::Task<Publisher::TrackStatusResult> MoqxRelay::trackStatusImpl(TrackStatus trackStatus
) {
  XLOG(DBG1) << __func__ << " ftn=" << trackStatus.fullTrackName;

  auto session = MoQSession::getRequestSession();

  if (trackStatus.fullTrackName.trackNamespace.empty() && !emptyNamespaceAllowed(session)) {
    co_return folly::makeUnexpected(TrackStatusError(
        {trackStatus.requestID, TrackStatusErrorCode::DOES_NOT_EXIST, "namespace required"}
    ));
  }

  auto upstreamView = registry_.getUpstreamView(trackStatus.fullTrackName);
  // Active subscription: answer from the publisher forwarder's state instead of going upstream.
  std::optional<TrackStatusOk> trackStatusOk;
  if (upstreamView) {
    trackStatusOk = co_await readLocalTrackStatus(*upstreamView, trackStatus);
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

  evictOnOwner(ftn, session, std::move(evict));
}

void MoqxRelay::evictOnOwner(
    const FullTrackName& ftn,
    const std::shared_ptr<MoQSession>& /*session*/,
    folly::Function<void(const std::shared_ptr<MoQForwarder>&)> evict
) {
  evict(registry_.getForwarderRef(ftn).getIfOwned());
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

// Parks (at most once) until a publisher resolves for ftn or `timeout` elapses. A wake
// is only a hint: if the publisher is gone again by the time we resume, the caller's
// ordinary no-publisher path reports it.
folly::coro::Task<std::optional<SubscribeError>> MoqxRelay::rendezvousWithPublisherOrTimeout(
    const SubscribeRequest& subReq,
    std::chrono::milliseconds timeout
) {
  const auto& ftn = subReq.fullTrackName;

  if (registry_.exists(ftn) || namespaceTree_.findPublisherSession(ftn.trackNamespace)) {
    co_return std::nullopt;
  }

  auto waiter = std::make_shared<moxygen::TimedBaton>();
  pendingRendezvous_.addWaiter(ftn, waiter);
  auto cleanup =
      folly::makeGuard([this, ftn, waiter] { pendingRendezvous_.eraseWaiter(ftn, waiter); });

  auto waitRes = co_await folly::coro::co_awaitTry(waiter->wait(timeout, timekeeper_.get()));

  if (waitRes.hasException()) {
    if (waitRes.template hasException<folly::FutureTimeout>()) {
      co_return SubscribeError{
          subReq.requestID,
          SubscribeErrorCode::TIMEOUT,
          "rendezvous timeout expired"
      };
    }
    if (waitRes.template hasException<folly::OperationCancelled>()) {
      co_yield folly::coro::co_stopped_may_throw;
    }
    co_return SubscribeError{
        subReq.requestID,
        SubscribeErrorCode::INTERNAL_ERROR,
        folly::to<std::string>("rendezvous wait failed: ", waitRes.exception().what().toStdString())
    };
  }

  co_return std::nullopt;
}

} // namespace openmoq::moqx
