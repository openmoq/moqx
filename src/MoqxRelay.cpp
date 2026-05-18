/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Originally from github.com/facebookexperimental/moxygen.
 * See deps/moxygen/LICENSE for the original license terms.
 *
 * Copyright (c) OpenMOQ contributors.
 */

#include "MoqxRelay.h"
#include "relay/MoQCrossExecFilter.h"
#include "relay/MoQCrossExecForwarderCallback.h"
#include "relay/MoQSubscriberCrossExecFilter.h"
#include "relay/WeakRelayForwarderCallback.h"
#include "relay/RelayExecUtil.h"
#include "relay/NullConsumers.h"
#include <folly/container/F14Set.h>
#include <moxygen/MoQFilters.h>
#include <moxygen/MoQTrackProperties.h>

namespace {
constexpr uint8_t kDefaultUpstreamPriority = 128;
constexpr std::chrono::seconds kUpstreamConnectWaitTimeout(5);
} // namespace

using namespace moxygen;

namespace openmoq::moqx {

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

  void namespaceMsg(const TrackNamespace& suffix) override {
    activeNamespaces_.insert(suffix);
    PublishNamespace pubNs;
    pubNs.trackNamespace = suffix;
    runOnExec(
        relayExec_,
        [relay = relay_, pubNs = std::move(pubNs), session = session_, peerID = peerID_]() mutable {
          if (auto r = relay.lock()) {
            r->doPublishNamespace(std::move(pubNs), session, nullptr, peerID);
          }
        }
    );
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
  // subscribeNamespace must run on the upstream session's executor
  auto result = co_await folly::coro::co_withExecutor(
      folly::getKeepAliveToken(session->getExecutor()),
      session->subscribeNamespace(makePeerSubNs(relayID_), nsHandle)
  );
  if (result.hasValue()) {
    upstreamSubNsHandle_ = std::move(result.value());
  } else {
    XLOG(ERR) << "MoqxRelay: upstream peer subNs failed: " << result.error().reasonPhrase;
  }
}

void MoqxRelay::onUpstreamDisconnect() {
  upstreamSubNsHandle_.reset();
}

// Sends SUBSCRIBE_UPDATE to update forwarding state. Called from:
// - subscribeNamespace: forwarder was empty, new subscriber added
// (forward=true)
// - subscribe: first forwarding subscriber added (forward=true)
// - onEmpty: last subscriber left a publish subscription (forward=false)
// - forwardChanged: forwarding subscriber count changed
folly::coro::Task<void>
MoqxRelay::doSubscribeUpdate(std::shared_ptr<Publisher::SubscriptionHandle> handle, bool forward) {
  auto updateRes = co_await handle->requestUpdate(
      {RequestID(0),
       handle->subscribeOk().requestID,
       kLocationMin,
       kLocationMax.group,
       kDefaultPriority,
       /*forward=*/forward}
  );
  if (updateRes.hasError()) {
    XLOG(ERR) << "requestUpdate failed: " << updateRes.error().reasonPhrase;
  }
}

// Sends a REQUEST_UPDATE that carries only the NEW_GROUP_REQUEST parameter.
folly::coro::Task<void> MoqxRelay::doNewGroupRequestUpdate(
    std::shared_ptr<Publisher::SubscriptionHandle> handle,
    uint64_t newGroupRequestValue
) {
  XLOG(DBG4) << "Sending NEW_GROUP_REQUEST update: " << newGroupRequestValue;
  RequestUpdate update;
  update.requestID = RequestID(0);
  update.existingRequestID = handle->subscribeOk().requestID;
  update.params.insertParam(
      Parameter(folly::to_underlying(TrackRequestParamKey::NEW_GROUP_REQUEST), newGroupRequestValue)
  );
  auto updateRes = co_await handle->requestUpdate(std::move(update));
  if (updateRes.hasError()) {
    XLOG(ERR) << "NEW_GROUP_REQUEST update failed: " << updateRes.error().reasonPhrase;
  }
}

std::shared_ptr<Subscriber::PublishNamespaceHandle> MoqxRelay::doPublishNamespace(
    PublishNamespace pubNs,
    std::shared_ptr<MoQSession> session,
    std::shared_ptr<Subscriber::PublishNamespaceCallback> callback,
    std::string peerID
) {
  XLOG(DBG1) << __func__ << " ns=" << pubNs.trackNamespace;
  // check auth
  if (!pubNs.trackNamespace.startsWith(allowedNamespacePrefix_)) {
    return nullptr;
  }
  auto [nodePtr, sessions, replacedSession] = namespaceTree_.setPublisher(
      pubNs.trackNamespace,
      session,
      std::move(callback),
      std::move(peerID),
      pubNs.requestID
  );
  if (replacedSession) {
    XLOG(WARNING) << "PublishNamespace: Existing session (" << replacedSession.get()
                  << ") has already published trackNamespace=" << pubNs.trackNamespace;
    // Remove ongoing subscriptions for the replaced publisher.
    registry_.removeIf([&](const FullTrackName& ftn, const SubscriptionRegistry::EntryView& e) {
      if (ftn.trackNamespace.startsWith(pubNs.trackNamespace) && e.upstream == replacedSession) {
        XLOG(DBG4) << "Erasing subscription to " << ftn;
        return true;
      }
      return false;
    });
  }
  for (auto& [outSession, info] : sessions) {
    if (outSession != session && (info.options == SubscribeNamespaceOptions::NAMESPACE ||
                                  info.options == SubscribeNamespaceOptions::BOTH)) {
      if (info.namespacePublishHandle) {
        // Draft 16+: send NAMESPACE message on the bidi stream.
        // Must run on the subscriber session's executor; dispatch when on relay exec.
        TrackNamespace suffix(std::vector<std::string>(
            pubNs.trackNamespace.trackNamespace.begin() + info.trackNamespacePrefix.size(),
            pubNs.trackNamespace.trackNamespace.end()
        ));
        auto handle = info.namespacePublishHandle;
        runOnSessionExec(
            relayExec_, outSession->getExecutor(),
            [handle, suffix = std::move(suffix)]() mutable { handle->namespaceMsg(suffix); }
        );
      } else {
        // Draft <= 15: send PUBLISH_NAMESPACE on a new stream
        auto exec = outSession->getExecutor();
        co_withExecutor(exec, publishNamespaceToSession(outSession, pubNs, nodePtr)).start();
      }
    }
  }
  return nodePtr;
}

folly::coro::Task<Subscriber::PublishNamespaceResult> MoqxRelay::publishNamespace(
    PublishNamespace pubNs,
    std::shared_ptr<Subscriber::PublishNamespaceCallback> callback
) {
  if (relayExec_) {
    co_return co_await folly::coro::co_withExecutor(
        folly::getKeepAliveToken(relayExec_),
        publishNamespaceImpl(std::move(pubNs), std::move(callback))
    );
  }
  co_return co_await publishNamespaceImpl(std::move(pubNs), std::move(callback));
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
    if (outSession != session && (info.options == SubscribeNamespaceOptions::NAMESPACE ||
                                  info.options == SubscribeNamespaceOptions::BOTH)) {
      auto maybeVersion = outSession->getNegotiatedVersion();
      if (maybeVersion.has_value() && getDraftMajorVersion(*maybeVersion) >= 16) {
        if (info.namespacePublishHandle) {
          TrackNamespace suffix(std::vector<std::string>(
              trackNamespace.trackNamespace.begin() + info.trackNamespacePrefix.size(),
              trackNamespace.trackNamespace.end()
          ));
          auto handle = info.namespacePublishHandle;
          runOnSessionExec(
              relayExec_, outSession->getExecutor(),
              [handle, suffix = std::move(suffix)]() mutable { handle->namespaceDoneMsg(suffix); }
          );
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

  // Clears handle + upstream; erases if no subscribers remain.
  auto forwarder = registry_.onPublisherTerminated(ftn);
  if (!forwarder) {
    XLOG(DBG1) << "Publisher terminated with no subscribers, cleaning up " << ftn;
  }
}

Subscriber::PublishResult
MoqxRelay::publish(PublishRequest pub, std::shared_ptr<Publisher::SubscriptionHandle> handle) {
  XLOG(DBG1) << __func__ << " ftn=" << pub.fullTrackName;
  XCHECK(handle) << "Publish handle cannot be null";
  // Validate before touching relay state (safe to do on calling thread).
  if (!pub.fullTrackName.trackNamespace.startsWith(allowedNamespacePrefix_)) {
    return folly::makeUnexpected(
        PublishError{pub.requestID, PublishErrorCode::UNINTERESTED, "bad namespace"}
    );
  }
  if (pub.fullTrackName.trackNamespace.empty()) {
    return folly::makeUnexpected(
        PublishError({pub.requestID, PublishErrorCode::INTERNAL_ERROR, "namespace required"})
    );
  }

  // Capture session here — getRequestSession() reads coroutine-local state
  // that must be read on the calling (I/O) thread before any executor switch.
  auto session = MoQSession::getRequestSession();

  if (relayExec_) {
    // Forwarder lives on publisherExec; relay chain added as channel sub (null
    // downstream). coPublish wires it to topNFilter before PUBLISH_OK.
    auto forwarder = std::make_shared<MoQForwarder>(pub.fullTrackName, pub.largest);
    forwarder->setExtensions(pub.extensions);
    auto relayChainFilter = std::make_shared<MoQCrossExecFilter>(relayExec_, nullptr);
    forwarder->addChannelSubscriber(relayExec_, /*forward=*/true, relayChainFilter);
    return PublishConsumerAndReplyTask{
        forwarder,
        coPublish(
            std::move(pub),
            std::move(handle),
            std::move(session),
            std::move(forwarder),
            std::move(relayChainFilter))};
  }

  auto setup = publishWithSession(std::move(pub), std::move(handle), std::move(session));
  if (setup.hasError()) {
    return folly::makeUnexpected(setup.error());
  }
  return PublishConsumerAndReplyTask{
      std::move(setup.value().consumer),
      folly::coro::makeTask<folly::Expected<PublishOk, PublishError>>(
          folly::Expected<PublishOk, PublishError>(std::move(setup.value().publishOk)))
  };
}

MoqxRelay::PublishSetupResult MoqxRelay::publishWithSession(
    PublishRequest pub,
    std::shared_ptr<Publisher::SubscriptionHandle> handle,
    std::shared_ptr<MoQSession> session,
    std::shared_ptr<MoQForwarder> forwarder
) {
  // Handle duplicate publisher at relay level before registering in the tree.
  // Move the forwarder out and erase the entry BEFORE calling publishDone.
  // publishDone iterates subscribers via forEachSubscriber; if a subscriber
  // has no open subgroups, drainSubscriber → onEmpty fires. If the entry
  // still existed, onEmpty would erase it (destroying the forwarder) while
  // forEachSubscriber is still iterating → use-after-free.
  // handle is null when the previous publisher already terminated (e.g.
  // reconnect after a dropped connection with subscribers still draining open
  // subgroups).  onPublishDone() already reset handle and drained the
  // forwarder, so skip both calls to avoid a null deref and a double
  // publishDone.
  if (!forwarder) {
    forwarder = std::make_shared<MoQForwarder>(pub.fullTrackName, pub.largest);
    forwarder->setExtensions(pub.extensions);
  }

  auto publishEntry = registry_.createFromPublish(
      pub.fullTrackName,
      forwarder,
      session,
      pub.requestID,
      std::move(handle),
      [&](std::shared_ptr<MoQForwarder> f) { return buildFilterChain(pub.fullTrackName, f); }
  );

  if (publishEntry.evicted) {
    XLOG(DBG1) << "New publisher for existing subscription";
    auto& evicted = *publishEntry.evicted;
    if (evicted.handle) {
      evicted.handle->unsubscribe();
      evicted.forwarder->publishDone(
          {RequestID(0),
           PublishDoneStatusCode::SUBSCRIPTION_ENDED,
           0, // filled in by session
           "upstream disconnect"}
      );
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
      XLOG(DBG4) << "added subscriber for ftn=" << forwarder->fullTrackName();
      auto* primaryExec = relayExec_ ? session->getExecutor() : nullptr;
      auto* sessionExec = primaryExec ? outSession->getExecutor() : nullptr;
      auto* dispatchExec = primaryExec ? primaryExec : outSession->getExecutor();
      co_withExecutor(
          dispatchExec,
          publishViaLocalForwarder(outSession, forwarder, sessionExec, info.forward, /*pinned=*/true)
      ).start();
    }
  }

  if (relayExec_) {
    auto relayAdapter = std::make_shared<WeakRelayForwarderCallback>(
        std::weak_ptr<moxygen::MoQForwarder::Callback>(shared_from_this()));
    forwarder->setCallback(std::make_shared<MoQCrossExecForwarderCallback>(
        relayExec_, forwarder, std::move(relayAdapter)));
  } else {
    forwarder->setCallback(shared_from_this());
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
          kLocationMax.group}
  };
}

folly::coro::Task<folly::Expected<PublishOk, PublishError>> MoqxRelay::coPublish(
    PublishRequest pub,
    std::shared_ptr<Publisher::SubscriptionHandle> handle,
    std::shared_ptr<MoQSession> session,
    std::shared_ptr<MoQForwarder> forwarder,
    std::shared_ptr<MoQCrossExecFilter> relayChainFilter
) {
  co_return co_await folly::coro::co_withExecutor(
      folly::getKeepAliveToken(relayExec_),
      [this,
       pub = std::move(pub),
       handle = std::move(handle),
       session = std::move(session),
       forwarder = std::move(forwarder),
       relayChainFilter = std::move(relayChainFilter)]() mutable
          -> folly::coro::Task<folly::Expected<PublishOk, PublishError>> {
        auto ftn = forwarder->fullTrackName(); // save before move
        // Direct call (not publish()) avoids re-entering the relayExec_ dispatch path.
        auto setup = publishWithSession(
            std::move(pub), std::move(handle), std::move(session), std::move(forwarder));
        if (setup.hasError()) {
          co_return folly::makeUnexpected(setup.error());
        }
        // Wire downstream now; FIFO on relayExec_ guarantees this before any data.
        auto topNView = registry_.getTopNView(ftn);
        if (topNView && topNView->topNFilter) {
          relayChainFilter->setDownstream(topNView->topNFilter);
        }
        co_return folly::Expected<PublishOk, PublishError>(std::move(setup.value().publishOk));
      }()
  );
}


// Runs on primaryExec. sessionExec non-null in multi-iothread mode: hops
// session->publish() to the subscriber's executor via MoQSubscriberCrossExecFilter.
folly::coro::Task<void> MoqxRelay::publishViaLocalForwarder(
    std::shared_ptr<MoQSession> session,
    std::shared_ptr<MoQForwarder> primaryFwd,
    folly::Executor* sessionExec,
    bool forward,
    bool pinned
) {
  if (session->isClosed()) {
    co_return;
  }

  auto subscriber = primaryFwd->addSubscriber(session, forward);
  if (!subscriber) {
    XLOG(ERR) << "publishViaLocalForwarder: addSubscriber returned null for "
              << primaryFwd->fullTrackName();
    co_return;
  }
  subscriber->pinned = pinned;

  // sessionExec set → hop publish() to subscriber's executor via MoQSubscriberCrossExecFilter,
  // which returns a placeholder consumer synchronously so primaryFwd can deliver before co_await.
  std::shared_ptr<MoQSubscriberCrossExecFilter> wrapped;
  auto pubInitial = [&]() {
    if (sessionExec) {
      wrapped = std::make_shared<MoQSubscriberCrossExecFilter>(sessionExec, session);
      return wrapped->publish(subscriber->getPublishRequest(), subscriber);
    }
    return session->publish(subscriber->getPublishRequest(), subscriber);
  }();

  if (pubInitial.hasError()) {
    XLOG(ERR) << "Publish failed err=" << pubInitial.error().reasonPhrase;
    subscriber->unsubscribe();
    co_return;
  }
  subscriber->trackConsumer = std::move(pubInitial->consumer);

  auto pubResult = co_await co_awaitTry(std::move(pubInitial->reply));
  if (pubResult.hasException()) {
    XLOG(ERR) << "Publish failed err=" << pubResult.exception().what();
    subscriber->unsubscribe();
    co_return;
  }
  if (pubResult.value().hasError()) {
    XLOG(ERR) << "Publish failed err=" << pubResult.value().error().reasonPhrase;
    subscriber->unsubscribe();
    co_return;
  }
  XLOG(DBG1) << "Publish OK sess=" << session.get();
  subscriber->onPublishOk(pubResult.value().value());
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

// Filter TrackConsumer that intercepts publishDone to clean up relay state.
// Holds a weak_ptr to avoid a reference cycle: relay owns RelaySubscription
// which owns the filter chain (TopNFilter→TerminationFilter), so a strong
// relay ref here would prevent the relay from ever being destroyed.
class MoqxRelay::TerminationFilter : public TrackConsumerFilter {
public:
  TerminationFilter(
      std::weak_ptr<MoqxRelay> relay,
      FullTrackName ftn,
      std::shared_ptr<TrackConsumer> downstream
  )
      : TrackConsumerFilter(std::move(downstream)), relay_(std::move(relay)), ftn_(std::move(ftn)) {
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
};

std::shared_ptr<TrackConsumer> MoqxRelay::getSubscribeWriteback(
    const FullTrackName& ftn,
    std::shared_ptr<TrackConsumer> consumer
) {
  auto baseConsumer =
      cache_ ? cache_->getSubscribeWriteback(ftn, std::move(consumer)) : std::move(consumer);
  auto termFilter =
      std::make_shared<TerminationFilter>(shared_from_this(), ftn, std::move(baseConsumer));
  return std::static_pointer_cast<TrackConsumer>(termFilter);
}

SubscriptionRegistry::FilterChainResult
MoqxRelay::buildFilterChain(const FullTrackName& ftn, std::shared_ptr<MoQForwarder> forwarder) {
  if (relayExec_) {
    // Multi-iothread: publisher writes directly to forwarder on primaryExec.
    // relayChainFilter (added by publish()) fans off to topNFilter/termination/cache.
    std::shared_ptr<TrackConsumer> chainEnd = cache_
        ? cache_->makePassiveConsumer(ftn)
        : std::make_shared<moxygen::NullTrackConsumer>();
    auto terminationFilter =
        std::make_shared<TerminationFilter>(shared_from_this(), ftn, std::move(chainEnd));
    auto topNFilter =
        std::make_shared<TopNFilter>(ftn, std::static_pointer_cast<TrackConsumer>(terminationFilter));
    topNFilter->setActivityThreshold(activityThreshold_);
    return SubscriptionRegistry::FilterChainResult{
        .consumer = std::static_pointer_cast<TrackConsumer>(forwarder),
        .topNFilter = topNFilter
    };
  }

  // Single-threaded: chain wraps forwarder directly (no cross-exec needed).
  // Cache attaches as a passive subscriber of the forwarder.
  if (cache_) {
    forwarder->addSubscriber(
        /*session=*/nullptr,
        /*forward=*/true,
        cache_->makePassiveConsumer(ftn),
        /*passive=*/true);
  }
  auto terminationFilter =
      std::make_shared<TerminationFilter>(shared_from_this(), ftn, std::static_pointer_cast<TrackConsumer>(forwarder));
  auto topNFilter =
      std::make_shared<TopNFilter>(ftn, std::static_pointer_cast<TrackConsumer>(terminationFilter));
  topNFilter->setActivityThreshold(activityThreshold_);
  return SubscriptionRegistry::FilterChainResult{
      .consumer = std::static_pointer_cast<TrackConsumer>(topNFilter),
      .topNFilter = topNFilter
  };
}

folly::coro::Task<Publisher::SubscribeNamespaceResult> MoqxRelay::subscribeNamespace(
    SubscribeNamespace subNs,
    std::shared_ptr<NamespacePublishHandle> namespacePublishHandle
) {
  if (relayExec_) {
    co_return co_await folly::coro::co_withExecutor(
        folly::getKeepAliveToken(relayExec_),
        subscribeNamespaceImpl(std::move(subNs), std::move(namespacePublishHandle))
    );
  }
  co_return co_await subscribeNamespaceImpl(std::move(subNs), std::move(namespacePublishHandle));
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
    // subscribeNamespace must run on the peer session's executor.
    auto recipResult = co_await folly::coro::co_withExecutor(
        folly::getKeepAliveToken(session->getExecutor()),
        session->subscribeNamespace(makePeerSubNs(), handle)
    ); // no token: reciprocal, prevents loop
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

  // check auth
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

  // Parse TRACK_FILTER parameter if present (SUBSCRIBE_NAMESPACE only)
  std::optional<TrackFilter> trackFilter;
  for (const auto& param : subNs.params) {
    if (param.key == folly::to_underlying(TrackRequestParamKey::TRACK_FILTER)) {
      trackFilter = param.asTrackFilter;
      break;
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
          trackFilter
      }
  );

  // If TRACK_FILTER is present, enroll session in PropertyRanking for top-N selection.
  // NOTE: onSelected callbacks fire synchronously within addSessionToTopNGroup() for
  // tracks already in top-N, triggering publishToSession() before this call returns.
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
        if (node->publisherSession() && node->publisherSession() != session &&
            (incomingPeerID.empty() || node->publisherPeerID() != incomingPeerID)) {
          if (getDraftMajorVersion(*maybeNegotiatedVersion) >= 16) {
            if (subNs.options == SubscribeNamespaceOptions::NAMESPACE ||
                subNs.options == SubscribeNamespaceOptions::BOTH) {
              // Compute the suffix: prefix minus subNs.trackNamespacePrefix
              TrackNamespace suffix(std::vector<std::string>(
                  prefix.trackNamespace.begin() + subNs.trackNamespacePrefix.size(),
                  prefix.trackNamespace.end()
              ));
              namespacePublishHandle->namespaceMsg(suffix);
            }
          } else {
            // TODO: Auth/params
            co_withExecutor(
                exec,
                publishNamespaceToSession(session, {subNs.requestID, prefix}, node)
            )
                .start();
          }
        }
        node->forEachPublish([&](const std::string& trackName,
                                 const std::shared_ptr<MoQSession>& publishSession) {
          FullTrackName ftn{prefix, trackName};
          auto forwarder = registry_.getForwarder(ftn);
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
              XLOG(DBG4) << "added subscriber for ftn=" << forwarder->fullTrackName();
              auto* primaryExec = relayExec_ ? publishSession->getExecutor() : nullptr;
              auto* sessionExec = primaryExec ? session->getExecutor() : nullptr;
              auto* dispatchExec = primaryExec ? primaryExec : session->getExecutor();
              co_withExecutor(
                  dispatchExec,
                  publishViaLocalForwarder(session, forwarder, sessionExec, subNs.forward, /*pinned=*/true)
              ).start();
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

MoqxRelay::PublishState MoqxRelay::findPublishState(const FullTrackName& ftn) {
  PublishState state;
  auto nodePtr = namespaceTree_.findNode(
      ftn.trackNamespace,
      /*createMissingNodes=*/false,
      NamespaceTree::MatchType::Exact
  );

  if (!nodePtr) {
    // Node doesn't exist - tree was properly pruned
    return state;
  }

  state.nodeExists = true;

  state.session = nodePtr->findPublishSession(ftn.trackName);

  return state;
}

// === Multi-iothread subscribe helpers ===

folly::coro::Task<MoqxRelay::StatefulSubscribeResult>
MoqxRelay::subscribeStatefulWork(SubscribeRequest subReq) {
  const auto& ftn = subReq.fullTrackName;

  if (!registry_.exists(ftn) && upstream_ &&
      !namespaceTree_.findPublisherSession(ftn.trackNamespace)) {
    co_await upstream_->waitForConnected(kUpstreamConnectWaitTimeout);
  }

  auto firstOrSubsequent = registry_.getOrCreateFromSubscribe(
      ftn,
      shared_from_this(),
      [this, &ftn](std::shared_ptr<MoQForwarder> f) { return buildFilterChain(ftn, std::move(f)); }
  );

  if (auto* first = std::get_if<SubscriptionRegistry::FirstSubscriber>(&firstOrSubsequent)) {
    auto upstreamSession = namespaceTree_.findPublisherSession(ftn.trackNamespace);
    if (!upstreamSession) {
      co_return StatefulSubscribeResult{
          nullptr, nullptr,
          folly::makeUnexpected(SubscribeError{
              subReq.requestID, SubscribeErrorCode::TRACK_NOT_EXIST, "no such namespace or track"}),
          std::nullopt};
    }

    const auto clientRequestID = subReq.requestID;
    SubscribeRequest upstreamSubReq = subReq;
    upstreamSubReq.priority = kDefaultUpstreamPriority;
    upstreamSubReq.groupOrder = GroupOrder::Default;
    upstreamSubReq.locType = LocationType::LargestObject;
    upstreamSubReq.forward = false; // updated to real value in completeFirstSubscriber
    upstreamSubReq.requestID = upstreamSession->peekNextRequestID();

    // first->consumer is the primary forwarder (lives on primaryExec == upstreamSession's
    // executor). No cross-exec wrapping — upstream delivers on that executor directly.
    auto upstreamConsumer = first->consumer;
    StatefulSubscribeResult result{
        first->forwarder, upstreamSession->getExecutor(), std::nullopt, std::nullopt};
    result.firstSetup.emplace(StatefulSubscribeResult::FirstSubscriberSetup{
        upstreamSession,
        std::move(upstreamSubReq),
        std::move(upstreamConsumer),
        std::move(first->pending),
        clientRequestID});
    co_return result;

  } else {
    auto sub = co_await std::get<folly::coro::Task<SubscriptionRegistry::SubsequentSubscriber>>(
        std::move(firstOrSubsequent)
    );
    auto upstreamView = registry_.getUpstreamView(ftn);
    auto* primaryExec = upstreamView && upstreamView->upstream
        ? upstreamView->upstream->getExecutor()
        : nullptr;
    co_return StatefulSubscribeResult{sub.forwarder, primaryExec, std::nullopt, std::nullopt};
  }
}


namespace {

// Free coroutine helpers for ChannelForwarderCallback — avoids lambda-coroutine
// capture lifetime issues (lambda lives on caller stack; coroutine frame outlives it).
folly::coro::Task<void> channelSubscribeUpdate(
    std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle, bool forward) {
  auto res = co_await handle->requestUpdate(RequestUpdate{
      RequestID(0),
      handle->subscribeOk().requestID,
      kLocationMin,
      kLocationMax.group,
      kDefaultPriority,
      forward});
  if (res.hasError()) {
    XLOG(ERR) << "requestUpdate failed: " << res.error().reasonPhrase;
  }
}

folly::coro::Task<void> channelNewGroupRequestUpdate(
    std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle, uint64_t group) {
  RequestUpdate update;
  update.requestID = RequestID(0);
  update.existingRequestID = handle->subscribeOk().requestID;
  update.params.insertParam(Parameter(
      folly::to_underlying(TrackRequestParamKey::NEW_GROUP_REQUEST), group));
  auto res = co_await handle->requestUpdate(std::move(update));
  if (res.hasError()) {
    XLOG(ERR) << "NEW_GROUP_REQUEST update failed: " << res.error().reasonPhrase;
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
// Multi-threaded — primary forwarder (lives on primaryExec):
//   primaryFwd.callback =
//     MoQCrossExecForwarderCallback(relayExec_, primaryFwd,
//       WeakRelayForwarderCallback(relay))
//
//   [primaryExec] MoQCrossExecForwarderCallback: captures ftn by value,
//                 dispatches to relayExec_ fire-and-forget
//       ↓
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
//         MoQCrossExecForwarderCallback(primaryExec, localFwd,
//           ChannelForwarderCallback(primaryFwd, subscriberExec, primaryExec)))
//
//   [subscriberExec] LocalForwarderCallback: removes from localReg on onEmpty,
//                    passes through forwardChanged / newGroupRequested
//       ↓ (MoQCrossExecForwarderCallback dispatches to primaryExec)
//   [primaryExec]    ChannelForwarderCallback:
//                      onEmpty → primaryFwd->removeChannelSubscriberByExec(subscriberExec)
//                                (may cascade into primaryFwd's own callback chain above)
//                      forwardChanged / newGroupRequested → launch background coro
//                                                           → requestUpdate(handle_)
//
// Weak-ptr discipline:
//   WeakRelayForwarderCallback holds weak_ptr<relay> to break the cycle
//   registry → forwarder → callback → relay → registry.
//   MoQCrossExecForwarderCallback holds weak_ptr<forwarder> so a destroyed
//   forwarder is detected on the target executor before dereferencing.

// Captures forwardChanged/newGroupRequested/onEmpty during setup (getOrCreate→setCallback window)
// so they can be replayed once the real callback is installed.
class PendingForwarderCallback : public moxygen::MoQForwarder::Callback {
public:
  PendingForwarderCallback(
      openmoq::moqx::LocalForwarderRegistry* localReg, moxygen::FullTrackName ftn)
      : localReg_(localReg), ftn_(std::move(ftn)) {}

  void forwardChanged(moxygen::MoQForwarder*, bool f) override { lastForward_ = f; }
  void newGroupRequested(moxygen::MoQForwarder*, uint64_t g) override {
    maxGroup_ = std::max(maxGroup_.value_or(0), g);
  }
  void onEmpty(moxygen::MoQForwarder*) override {
    localReg_->remove(ftn_);
    sawOnEmpty_ = true;
  }

  openmoq::moqx::LocalForwarderRegistry* localReg_;
  moxygen::FullTrackName ftn_;
  std::optional<bool> lastForward_;
  std::optional<uint64_t> maxGroup_;
  bool sawOnEmpty_{false};
};

// Runs on the subscriber's executor. Removes the thread-local forwarder entry,
// snapshots the current forward state (safe to read here), then delegates to
// downstream (a MoQCrossExecForwarderCallback targeting the primary's executor).
class LocalForwarderCallback : public moxygen::MoQForwarder::Callback {
public:
  LocalForwarderCallback(
      openmoq::moqx::LocalForwarderRegistry* localReg,
      moxygen::FullTrackName ftn,
      std::shared_ptr<moxygen::MoQForwarder::Callback> downstream
  )
      : localReg_(localReg), ftn_(std::move(ftn)), downstream_(std::move(downstream)) {}

  void onEmpty(moxygen::MoQForwarder* forwarder) override {
    localReg_->remove(ftn_);
    downstream_->onEmpty(forwarder);
  }

  void forwardChanged(moxygen::MoQForwarder* forwarder, bool forward) override {
    downstream_->forwardChanged(forwarder, forward);
  }

  void newGroupRequested(moxygen::MoQForwarder* forwarder, uint64_t group) override {
    downstream_->newGroupRequested(forwarder, group);
  }

private:
  openmoq::moqx::LocalForwarderRegistry* localReg_;
  moxygen::FullTrackName ftn_;
  std::shared_ptr<moxygen::MoQForwarder::Callback> downstream_;
};

// Runs on the primary forwarder's executor (publisher's iothread). Propagates
// channel-subscriber lifecycle events to the primary and upstream handle.
class ChannelForwarderCallback : public moxygen::MoQForwarder::Callback {
public:
  ChannelForwarderCallback(
      std::weak_ptr<moxygen::MoQForwarder> weakPrimary,
      folly::Executor* subscriberExec,
      folly::Executor* primaryExec
  )
      : weakPrimary_(std::move(weakPrimary)), subscriberExec_(subscriberExec),
        primaryExec_(primaryExec) {}

  // Called on primaryExec immediately after addChannelSubscriber returns.
  void setHandle(std::shared_ptr<moxygen::Publisher::SubscriptionHandle> h) {
    handle_ = std::move(h);
  }

  void onEmpty(moxygen::MoQForwarder*) override {
    if (auto primary = weakPrimary_.lock()) {
      primary->removeChannelSubscriberByExec(subscriberExec_);
    }
  }

  void forwardChanged(moxygen::MoQForwarder*, bool forward) override {
    if (!handle_) {
      return;
    }
    folly::coro::co_withExecutor(
        folly::getKeepAliveToken(primaryExec_),
        channelSubscribeUpdate(handle_, forward)
    ).start();
  }

  void newGroupRequested(moxygen::MoQForwarder*, uint64_t group) override {
    if (!handle_) {
      return;
    }
    folly::coro::co_withExecutor(
        folly::getKeepAliveToken(primaryExec_),
        channelNewGroupRequestUpdate(handle_, group)
    ).start();
  }

private:
  std::weak_ptr<moxygen::MoQForwarder> weakPrimary_;
  folly::Executor* subscriberExec_;
  folly::Executor* primaryExec_;
  std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle_;
};
} // namespace

folly::coro::Task<Publisher::SubscribeResult> MoqxRelay::subscribeFromSubscriberThread(
    SubscribeRequest subReq,
    std::shared_ptr<TrackConsumer> consumer,
    std::shared_ptr<MoQSession> session,
    folly::Executor* subscriberExec
) {
  const auto& ftn = subReq.fullTrackName;

  // Fast path: local forwarder already exists on this thread.
  if (auto* localReg = tlForwarders_.get()) {
    if (auto localFwd = localReg->get(ftn)) {
      if (localFwd->largest() && subReq.locType == LocationType::AbsoluteRange &&
          subReq.endGroup < localFwd->largest()->group) {
        co_return folly::makeUnexpected(SubscribeError{
            subReq.requestID, SubscribeErrorCode::INVALID_RANGE, "Range in the past, use FETCH"
        });
      }
      auto sub = localFwd->addSubscriber(session, subReq, std::move(consumer));
      if (!sub) {
        co_return folly::makeUnexpected(SubscribeError{
            subReq.requestID, SubscribeErrorCode::INTERNAL_ERROR, "failed to add subscriber"
        });
      }
      localFwd->tryProcessNewGroupRequest(subReq.params);
      co_return sub;
    }
  }

  // Ensure thread-local registry.
  if (!tlForwarders_.get()) {
    tlForwarders_.reset(new LocalForwarderRegistry());
  }
  auto* localReg = tlForwarders_.get();

  // getOrCreate before the relay hop: serializes same-iothread races.
  // isNew=true means this thread owns setup; isNew=false means another setup is in
  // progress (or complete) on this thread and we can just attach.
  auto [localFwd, isNew] = localReg->getOrCreate(
      ftn, [&] { return std::make_shared<MoQForwarder>(ftn); });

  if (!isNew) {
    if (localFwd->largest() && subReq.locType == LocationType::AbsoluteRange &&
        subReq.endGroup < localFwd->largest()->group) {
      co_return folly::makeUnexpected(SubscribeError{
          subReq.requestID, SubscribeErrorCode::INVALID_RANGE, "Range in the past, use FETCH"
      });
    }
    auto sub = localFwd->addSubscriber(session, subReq, std::move(consumer));
    if (!sub) {
      co_return folly::makeUnexpected(SubscribeError{
          subReq.requestID, SubscribeErrorCode::INTERNAL_ERROR, "failed to add subscriber"
      });
    }
    localFwd->tryProcessNewGroupRequest(subReq.params);
    co_return sub;
  }

  // isNew=true: this thread owns setup. Install PendingForwarderCallback first so
  // forwardChanged/newGroupRequested/onEmpty events during setup are captured for replay.
  auto pendingCb =
      std::make_shared<PendingForwarderCallback>(localReg, ftn);
  localFwd->setCallback(pendingCb);

  auto crossExecFilter = std::make_shared<MoQCrossExecFilter>(subscriberExec, localFwd);

  // addSubscriber before the relay hop: numForwardingSubscribers() must be correct
  // when addChannelSubscriber runs on primaryExec, so forward flag is right from the start.
  auto sub = localFwd->addSubscriber(session, subReq, std::move(consumer));
  if (!sub) {
    localReg->remove(ftn);
    co_return folly::makeUnexpected(SubscribeError{
        subReq.requestID, SubscribeErrorCode::INTERNAL_ERROR, "failed to add subscriber"
    });
  }
  bool forward = (localFwd->numForwardingSubscribers() > 0);

  // Out-vars set by the relay/primary chain, read after unwind on subscriberExec.
  std::optional<Publisher::SubscribeResult> upstreamError;
  struct UpstreamOk {
    std::shared_ptr<Publisher::SubscriptionHandle> handle;
    RequestID requestID;
    Extensions extensions;
  };
  std::optional<UpstreamOk> upstreamOk;
  std::shared_ptr<MoQForwarder::Callback> finalCallback;
  std::shared_ptr<MoQForwarder> primaryFwd;
  folly::Executor* primaryExec{nullptr};
  // Set on firstSetup path; wired to topNFilter on relayExec_ before pending.complete().
  std::shared_ptr<MoQCrossExecFilter> relayChainFilter;

  // Single hop to relayExec_, with a nested hop to primaryExec so that the relay-exec
  // cleanup (cache + pending.complete) runs on the natural unwind rather than as a
  // separate explicit hop.
  co_await folly::coro::co_withExecutor(
      folly::getKeepAliveToken(relayExec_),
      [&]() -> folly::coro::Task<void> {
        auto sr = co_await subscribeStatefulWork(subReq);
        if (sr.error) {
          upstreamError = std::move(*sr.error); // SubscribeResult (Expected with error)
          co_return;
        }

        primaryFwd = sr.primaryForwarder;
        primaryExec = sr.primaryExec;

        auto channelCb = std::make_shared<ChannelForwarderCallback>(
            primaryFwd, subscriberExec, primaryExec);
        auto crossExecCb = std::make_shared<MoQCrossExecForwarderCallback>(
            primaryExec, localFwd, channelCb);
        finalCallback = std::make_shared<LocalForwarderCallback>(
            localReg, ftn, std::move(crossExecCb));

        if (primaryFwd && primaryExec) {
          co_await folly::coro::co_withExecutor(
              folly::getKeepAliveToken(primaryExec),
              [&]() -> folly::coro::Task<void> {
                auto chanHandle = primaryFwd->addChannelSubscriber(
                    subscriberExec, forward, crossExecFilter);
                if (chanHandle) {
                  channelCb->setHandle(chanHandle);
                }
                if (!sr.firstSetup) {
                  co_return;
                }
                // Add relay-exec channel sub (topNFilter/terminationFilter/cache path).
                // setDownstream wired on relayExec_ before pending.complete().
                relayChainFilter = std::make_shared<MoQCrossExecFilter>(relayExec_, nullptr);
                primaryFwd->addChannelSubscriber(relayExec_, /*forward=*/true, relayChainFilter);
                auto& setup = *sr.firstSetup;
                setup.upstreamSubReq.forward = forward;
                auto subRes = co_await setup.upstreamSession->subscribe(
                    setup.upstreamSubReq, std::move(setup.upstreamConsumer));
                if (subRes.hasError()) {
                  upstreamError = folly::makeUnexpected(SubscribeError{
                      setup.clientRequestID,
                      subRes.error().errorCode,
                      folly::to<std::string>(
                          "upstream subscribe failed: ", subRes.error().reasonPhrase)});
                  co_return;
                }
                // Process subscribeOk on primaryExec (forwarder's executor).
                const auto& ok = subRes.value()->subscribeOk();
                auto reqID = ok.requestID;
                auto exts = ok.extensions; // copy before move of subRes
                if (ok.largest) {
                  primaryFwd->updateLargest(ok.largest->group, ok.largest->object);
                }
                primaryFwd->setExtensions(exts);
                primaryFwd->tryProcessNewGroupRequest(
                    setup.upstreamSubReq.params, /*fire=*/false);
                upstreamOk =
                    UpstreamOk{std::move(subRes.value()), reqID, std::move(exts)};
              }()
          );
        }

        // Natural unwind: back on relayExec_.
        if (upstreamError) {
          // Channel subscriber(s) installed before subscribe failed; clean up.
          if (primaryFwd && primaryExec) {
            auto re = relayExec_;
            auto rcf = relayChainFilter;
            folly::via(
                primaryExec,
                [pf = primaryFwd, ex = subscriberExec, re, rcf = std::move(rcf)]() noexcept {
                  pf->removeChannelSubscriberByExec(ex);
                  if (rcf) {
                    pf->removeChannelSubscriberByExec(re);
                  }
                });
          }
          // pending destructor fires when sr is destroyed, cleaning registry entry.
          co_return;
        }
        if (!sr.firstSetup) {
          co_return;
        }
        // Wire relay chain before pending.complete() so buffered objects see topNFilter.
        if (relayChainFilter) {
          auto topNView = registry_.getTopNView(ftn);
          if (topNView && topNView->topNFilter) {
            relayChainFilter->setDownstream(topNView->topNFilter);
          }
        }
        if (upstreamOk) {
          if (cache_) {
            cache_->setTrackExtensions(ftn, upstreamOk->extensions);
          }
          auto& setup = *sr.firstSetup;
          if (!setup.pending.complete(
                  std::move(upstreamOk->handle),
                  upstreamOk->requestID,
                  setup.upstreamSession)) {
            upstreamError = folly::makeUnexpected(SubscribeError{
                setup.clientRequestID,
                SubscribeErrorCode::INTERNAL_ERROR,
                "publisher reconnected during subscribe"});
          }
        }
      }()
  );

  // Natural unwind: back on subscriberExec.

  if (pendingCb->sawOnEmpty_) {
    // All subscribers left during setup; localReg entry already removed in
    // PendingForwarderCallback::onEmpty.
    // TODO: "everyone left during setup" and "everyone left + new subscriber joined"
    // are edge cases that need further scrutiny and tests. For now do a best-effort
    // cleanup of the channel subscriber if it was installed, and return an error.
    // The relay registry entry (and upstream subscription if pending.complete ran)
    // remains, so a subsequent subscribe on any thread will hit the SubsequentSubscriber
    // path and work correctly.
    if (primaryFwd && primaryExec) {
      auto re = relayExec_;
      auto rcf = relayChainFilter;
      folly::via(primaryExec, [pf = primaryFwd, ex = subscriberExec, re, rcf = std::move(rcf)]() noexcept {
        pf->removeChannelSubscriberByExec(ex);
        if (rcf) {
          pf->removeChannelSubscriberByExec(re);
        }
      });
    }
    co_return folly::makeUnexpected(SubscribeError{
        subReq.requestID, SubscribeErrorCode::INTERNAL_ERROR,
        "all subscribers cancelled during setup"});
  }

  if (upstreamError) {
    localFwd->publishDone(PublishDone{
        RequestID(0),
        PublishDoneStatusCode::INTERNAL_ERROR,
        0,
        upstreamError->error().reasonPhrase});
    localReg->remove(ftn);
    co_return std::move(*upstreamError);
  }

  // Install the real callback and replay any events that fired during the setup window.
  localFwd->setCallback(finalCallback);
  if (pendingCb->lastForward_ && *pendingCb->lastForward_ != forward) {
    finalCallback->forwardChanged(localFwd.get(), *pendingCb->lastForward_);
  }
  if (pendingCb->maxGroup_) {
    finalCallback->newGroupRequested(localFwd.get(), *pendingCb->maxGroup_);
  }

  localFwd->tryProcessNewGroupRequest(subReq.params);
  co_return sub;
}

// === End multi-iothread subscribe helpers ===

folly::coro::Task<Publisher::SubscribeResult>
MoqxRelay::subscribe(SubscribeRequest subReq, std::shared_ptr<TrackConsumer> consumer) {
  auto session = MoQSession::getRequestSession();
  if (subReq.fullTrackName.trackNamespace.empty()) {
    co_return folly::makeUnexpected(SubscribeError(
        {subReq.requestID, SubscribeErrorCode::TRACK_NOT_EXIST, "namespace required"}
    ));
  }
  if (relayExec_) {
    folly::Executor* subscriberExec = session->getExecutor();
    co_return co_await subscribeFromSubscriberThread(
        std::move(subReq), std::move(consumer), std::move(session), subscriberExec
    );
  }
  co_return co_await subscribeImpl(std::move(subReq), std::move(consumer));
}

folly::coro::Task<Publisher::SubscribeResult>
MoqxRelay::subscribeImpl(SubscribeRequest subReq, std::shared_ptr<TrackConsumer> consumer) {
  auto session = MoQSession::getRequestSession();
  const auto& ftn = subReq.fullTrackName;

  if (ftn.trackNamespace.empty()) {
    co_return folly::makeUnexpected(SubscribeError(
        {subReq.requestID, SubscribeErrorCode::TRACK_NOT_EXIST, "namespace required"}
    ));
  }

  // TOCTOU fix: if we might be the first subscriber, wait for the upstream
  // connection before branching. A concurrent coroutine may emplace the entry
  // while we are suspended, so we re-check inside getOrCreateFromSubscribe.
  if (!registry_.exists(ftn) && upstream_ &&
      !namespaceTree_.findPublisherSession(ftn.trackNamespace)) {
    co_await upstream_->waitForConnected(kUpstreamConnectWaitTimeout);
  }

  auto firstOrSubsequent = registry_.getOrCreateFromSubscribe(
      ftn,
      shared_from_this(),
      [this, &ftn](std::shared_ptr<MoQForwarder> f) { return buildFilterChain(ftn, std::move(f)); }
  );

  if (auto* first = std::get_if<SubscriptionRegistry::FirstSubscriber>(&firstOrSubsequent)) {
    auto upstreamSession = namespaceTree_.findPublisherSession(ftn.trackNamespace);
    if (!upstreamSession) {
      co_return folly::makeUnexpected(SubscribeError(
          {subReq.requestID, SubscribeErrorCode::TRACK_NOT_EXIST, "no such namespace or track"}
      ));
    } // pending destructor fires on early return above

    // Add subscriber first (with the client's original request) in case objects
    // arrive before subscribe OK.
    // When running with relay exec, the forwarder delivers from relayExec_.
    // Wrap consumer so data writes are dispatched to the subscriber's session executor.
    auto wrappedConsumer = maybeCrossExec(
        relayExec_ ? session->getExecutor() : nullptr, std::move(consumer)
    );
    auto subscriber =
        first->forwarder->addSubscriber(std::move(session), subReq, std::move(wrappedConsumer));
    if (!subscriber) {
      XLOG(ERR) << "addSubscriber returned null (draining?) for " << ftn
                << " reqID=" << subReq.requestID;
      co_return folly::makeUnexpected(SubscribeError{
          subReq.requestID,
          SubscribeErrorCode::INTERNAL_ERROR,
          "failed to add subscriber"
      });
    }
    XLOG(DBG4) << "added subscriber for ftn=" << ftn;

    // Override fields for the upstream subscribe: always request from latest,
    // upstream priority, and default group order.
    const auto clientRequestID = subReq.requestID;
    subReq.priority = kDefaultUpstreamPriority;
    subReq.groupOrder = GroupOrder::Default;
    subReq.locType = LocationType::LargestObject;
    // Per the spec, we're supposed to always forward=1 upstream
    subReq.forward = first->forwarder->numForwardingSubscribers() > 0;
    subReq.requestID = upstreamSession->peekNextRequestID();

    // Wrap the writeback so upstream data arrives back on relayExec_.
    // Then dispatch the subscribe call itself to the upstream session's executor.
    auto upstreamConsumer = maybeCrossExec(relayExec_, first->consumer);
    auto subRes = co_await folly::coro::co_withExecutor(
        folly::getKeepAliveToken(upstreamSession->getExecutor()),
        upstreamSession->subscribe(subReq, std::move(upstreamConsumer))
    );
    if (subRes.hasError()) {
      co_return folly::makeUnexpected(SubscribeError(
          {clientRequestID,
           subRes.error().errorCode,
           folly::to<std::string>("upstream subscribe failed: ", subRes.error().reasonPhrase)}
      ));
    } // pending destructor fires on error

    auto largest = subRes.value()->subscribeOk().largest;
    if (largest) {
      first->forwarder->updateLargest(largest->group, largest->object);
      subscriber->updateLargest(*largest);
    }
    auto& upstreamExtensions = subRes.value()->subscribeOk().extensions;
    first->forwarder->setExtensions(upstreamExtensions);
    if (cache_) {
      cache_->setTrackExtensions(ftn, upstreamExtensions);
    }

    // Record NGR as outstanding (no fire — it rides the outgoing SUBSCRIBE).
    first->forwarder->tryProcessNewGroupRequest(subReq.params, /*fire=*/false);

    auto requestID = subRes.value()->subscribeOk().requestID;
    if (!first->pending.complete(std::move(subRes.value()), requestID, upstreamSession)) {
      XLOG(ERR) << "Subscription replaced by reconnecting publisher: " << ftn;
      co_return folly::makeUnexpected(SubscribeError{
          clientRequestID,
          SubscribeErrorCode::INTERNAL_ERROR,
          "publisher reconnected during subscribe"
      });
    }
    co_return subscriber;

  } else {
    auto sub = co_await std::get<folly::coro::Task<SubscriptionRegistry::SubsequentSubscriber>>(
        std::move(firstOrSubsequent)
    );
    // sub.forwarder is valid: promise was fulfilled by first subscriber
    if (sub.forwarder->largest() && subReq.locType == LocationType::AbsoluteRange &&
        subReq.endGroup < sub.forwarder->largest()->group) {
      co_return folly::makeUnexpected(SubscribeError{
          subReq.requestID,
          SubscribeErrorCode::INVALID_RANGE,
          "Range in the past, use FETCH"
      });
    }
    consumer = maybeCrossExec(relayExec_ ? session->getExecutor() : nullptr, std::move(consumer));
    auto subscriber = sub.forwarder->addSubscriber(std::move(session), subReq, std::move(consumer));
    if (!subscriber) {
      XLOG(ERR) << "addSubscriber returned null (draining?) for " << ftn
                << " reqID=" << subReq.requestID;
      co_return folly::makeUnexpected(SubscribeError{
          subReq.requestID,
          SubscribeErrorCode::INTERNAL_ERROR,
          "failed to add subscriber"
      });
    }
    XLOG(DBG4) << "added subscriber for ftn=" << ftn;
    sub.forwarder->tryProcessNewGroupRequest(subReq.params);
    co_return subscriber;
  }
}

folly::coro::Task<Publisher::FetchResult>
MoqxRelay::fetch(Fetch fetch, std::shared_ptr<FetchConsumer> consumer) {
  if (relayExec_) {
    co_return co_await folly::coro::co_withExecutor(
        folly::getKeepAliveToken(relayExec_),
        fetchImpl(std::move(fetch), std::move(consumer))
    );
  }
  co_return co_await fetchImpl(std::move(fetch), std::move(consumer));
}

folly::coro::Task<Publisher::FetchResult>
MoqxRelay::fetchImpl(Fetch fetch, std::shared_ptr<FetchConsumer> consumer) {
  auto session = MoQSession::getRequestSession();

  // check auth
  // get trackNamespace
  if (fetch.fullTrackName.trackNamespace.empty()) {
    co_return folly::makeUnexpected(
        FetchError({fetch.requestID, FetchErrorCode::TRACK_NOT_EXIST, "namespace required"})
    );
  }

  auto [standalone, joining] = fetchType(fetch);
  if (joining) {
    auto fetchView = registry_.getFetchView(fetch.fullTrackName);
    if (!fetchView) {
      XLOG(ERR) << "No subscription for joining fetch";
      co_return folly::makeUnexpected(FetchError(
          {fetch.requestID, FetchErrorCode::TRACK_NOT_EXIST, "No subscription for joining fetch"}
      ));
    } else if (fetchView->isReady) {
      auto res = fetchView->forwarder->resolveJoiningFetch(session, *joining);
      if (res.hasError()) {
        co_return folly::makeUnexpected(res.error());
      }
      fetch.args = StandaloneFetch(res.value().start, res.value().end);
      joining = nullptr;
    } else {
      // Upstream is resolving the subscribe, forward joining fetch
      joining->joiningRequestID = fetchView->requestID;
    }
  }

  auto upstreamSession = namespaceTree_.findPublisherSession(fetch.fullTrackName.trackNamespace);
  if (!upstreamSession && upstream_) {
    co_await upstream_->waitForConnected(kUpstreamConnectWaitTimeout);
    upstreamSession = namespaceTree_.findPublisherSession(fetch.fullTrackName.trackNamespace);
  }
  if (!upstreamSession) {
    // Attempt to find matching upstream subscription (from publish)
    if (auto fetchView = registry_.getFetchView(fetch.fullTrackName)) {
      upstreamSession = fetchView->upstream;
    }
    if (!upstreamSession) {
      co_return folly::makeUnexpected(
          FetchError({fetch.requestID, FetchErrorCode::TRACK_NOT_EXIST, "no upstream for fetch"})
      );
    }
  }
  if (session.get() == upstreamSession.get()) {
    co_return folly::makeUnexpected(
        FetchError({fetch.requestID, FetchErrorCode::INTERNAL_ERROR, "self fetch"})
    );
  }
  fetch.priority = kDefaultUpstreamPriority;

  // Wrap consumer so upstream data is dispatched to the subscriber's executor.
  auto wrappedConsumer = maybeCrossExec(
      relayExec_ ? session->getExecutor() : nullptr, std::move(consumer)
  );

  if (!cache_ || joining) {
    // We can't use the cache on an unresolved joining fetch - we don't know
    // which objects are being requested.  However, once we have that resolved,
    // we SHOULD be able to serve from cache.
    if (standalone) {
      XLOG(DBG1) << "Upstream fetch {" << standalone->start.group << "," << standalone->start.object
                 << "}.." << standalone->end.group << "," << standalone->end.object << "}";
    }
    // Dispatch fetch call to the upstream session's executor.
    co_return co_await folly::coro::co_withExecutor(
        folly::getKeepAliveToken(upstreamSession->getExecutor()),
        upstreamSession->fetch(fetch, std::move(wrappedConsumer))
    );
  }
  // MoqxCache takes a Publisher, so ensure any upstream->fetch() calls it
  // issues are dispatched to the upstream session's own executor.
  std::shared_ptr<Publisher> upstream = upstreamSession;
  upstream = maybeCrossExec(relayExec_ ? upstreamSession->getExecutor() : nullptr, std::move(upstream));
  co_return co_await cache_->fetch(fetch, std::move(wrappedConsumer), std::move(upstream));
}

folly::coro::Task<Publisher::TrackStatusResult> MoqxRelay::trackStatus(TrackStatus trackStatus) {
  if (relayExec_) {
    co_return co_await folly::coro::co_withExecutor(
        folly::getKeepAliveToken(relayExec_), trackStatusImpl(std::move(trackStatus))
    );
  }
  co_return co_await trackStatusImpl(std::move(trackStatus));
}

folly::coro::Task<Publisher::TrackStatusResult>
MoqxRelay::trackStatusImpl(TrackStatus trackStatus) {
  XLOG(DBG1) << __func__ << " ftn=" << trackStatus.fullTrackName;

  if (trackStatus.fullTrackName.trackNamespace.empty()) {
    co_return folly::makeUnexpected(TrackStatusError(
        {trackStatus.requestID, TrackStatusErrorCode::TRACK_NOT_EXIST, "namespace required"}
    ));
  }

  auto upstreamView = registry_.getUpstreamView(trackStatus.fullTrackName);
  if (upstreamView && upstreamView->forwarder->numForwardingSubscribers() > 0) {
    // We have active subscription - answer directly from local forwarder state
    auto& forwarder = upstreamView->forwarder;

    TrackStatusCode statusCode = TrackStatusCode::TRACK_NOT_STARTED;
    // forwarder->largest() being set means: we have actually
    // received at least one object for this track.
    // upstreamView->handle being non-null means: the relay still has a
    // live upstream Publisher::SubscriptionHandle for this track
    if (forwarder->largest()) {
      if (upstreamView->handle) {
        statusCode = TrackStatusCode::IN_PROGRESS;
      } else {
        statusCode = TrackStatusCode::UNKNOWN;
      }
    }

    TrackStatusOk trackStatusOk;
    trackStatusOk.requestID = trackStatus.requestID;
    trackStatusOk.groupOrder = forwarder->groupOrder();
    trackStatusOk.largest = forwarder->largest();
    trackStatusOk.fullTrackName = trackStatus.fullTrackName;
    trackStatusOk.statusCode = statusCode;

    XLOG(DBG1) << "Returning local track status for " << trackStatus.fullTrackName
               << " statusCode=" << (uint32_t)statusCode;
    co_return trackStatusOk;
  } else {
    // No subscription - forward to upstream
    auto upstreamSession =
        namespaceTree_.findPublisherSession(trackStatus.fullTrackName.trackNamespace);
    if (!upstreamSession && upstream_) {
      co_await upstream_->waitForConnected(kUpstreamConnectWaitTimeout);
      upstreamSession =
          namespaceTree_.findPublisherSession(trackStatus.fullTrackName.trackNamespace);
    }
    if (!upstreamSession) {
      XLOG(DBG1) << "No upstream session for track: " << trackStatus.fullTrackName;
      co_return folly::makeUnexpected(TrackStatusError{
          trackStatus.requestID,
          TrackStatusErrorCode::TRACK_NOT_EXIST,
          "no such namespace or track"
      });
    }

    // Forward the trackStatus request to the upstream publisher session.
    auto result = co_await folly::coro::co_withExecutor(
        folly::getKeepAliveToken(upstreamSession->getExecutor()),
        upstreamSession->trackStatus(trackStatus)
    );

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

  // Handle exists - just last subscriber left
  XLOG(INFO) << "Last subscriber removed for " << ftn;
  if (upstreamView->isPublish) {
    // if it's publish, don't unsubscribe, just subscribeUpdate forward=false
    XLOG(DBG1) << "Updating upstream subscription forward=false";
    auto exec = upstreamView->upstream->getExecutor();
    co_withExecutor(exec, doSubscribeUpdate(upstreamView->handle, /*forward=*/false)).start();
  } else {
    upstreamView->handle->unsubscribe();
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

  auto exec = upstreamView->upstream->getExecutor();
  co_withExecutor(
      exec,
      doSubscribeUpdate(upstreamView->handle, forward)
  )
      .start();
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

  auto exec = upstreamView->upstream->getExecutor();
  auto handle = upstreamView->handle;
  co_withExecutor(exec, doNewGroupRequestUpdate(std::move(handle), group)).start();
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
              initialPropertyValue =
                  topNView->forwarder->extensions().getIntExtension(propertyType);
              // Wire value-change, track-ended, and activity observers on the existing TopNFilter.
              if (topNView->topNFilter) {
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

  if (!session || session->isClosed()) {
    XLOG(ERR) << "onTrackSelected: session null or closed, skipping " << ftn;
    return;
  }

  auto trackForwarder = registry_.getForwarder(ftn);
  if (!trackForwarder) {
    XLOG(DBG4) << "onTrackSelected: no subscription/forwarder for " << ftn;
    return;
  }

  auto exec = session->getExecutor();
  XCHECK(exec) << "onTrackSelected: null executor for session " << session.get();

  // TODO: Consider batching multiple publishViaLocalForwarder calls on the same
  // executor when multiple tracks are selected for the same session in one ranking update.
  auto upstreamView = registry_.getUpstreamView(ftn);
  auto* primaryExec = relayExec_ && upstreamView && upstreamView->upstream
      ? upstreamView->upstream->getExecutor()
      : nullptr;
  auto* sessionExec = primaryExec ? exec : nullptr;
  auto* dispatchExec = primaryExec ? primaryExec : exec;
  XLOG(DBG4) << "added subscriber for ftn=" << ftn;
  co_withExecutor(
      dispatchExec,
      publishViaLocalForwarder(session, trackForwarder, sessionExec, forward, /*pinned=*/false)
  ).start();
}

void MoqxRelay::onTrackEvicted(const FullTrackName& ftn, std::shared_ptr<MoQSession> session) {
  XLOG(DBG4) << "[MoqxRelay] Track evicted: " << ftn << " session=" << session.get();

  if (!session || session->isClosed()) {
    XLOG(WARN) << "onTrackEvicted: session null or closed, skipping " << ftn;
    return;
  }

  auto forwarder = registry_.getForwarder(ftn);
  if (!forwarder) {
    return;
  }
  auto sub = forwarder->getSubscriber(session.get());
  if (!sub || sub->isPinned()) {
    XLOG(DBG4) << "onTrackEvicted: pinned subscriber, skipping";
    return;
  }
  // Pass PublishDone so removeSubscriber calls trackConsumer->publishDone() before
  // removing the subscriber. Passing nullopt would silently drop the subscriber
  // without notifying the downstream session that its subscription ended.
  forwarder->removeSubscriber(
      session,
      PublishDone{RequestID(0), PublishDoneStatusCode::SUBSCRIPTION_ENDED, 0, "evicted"},
      "onTrackEvicted"
  );
}

void MoqxRelay::dumpState(RelayStateVisitor& visitor) const {
  visitor.onPeersBegin();
  for (const auto& [sess, peer] : peerSubNsHandles_) {
    visitor.onPeer(sess->getPeerAddress().describe(), sess->getAuthority(), peer.relayID);
  }
  visitor.onPeersEnd();

  visitor.onSubscriptionsBegin();
  registry_.forEach([&](const SubscriptionRegistry::EntryView& e) {
    std::string sourceAddr;
    if (e.upstream) {
      sourceAddr = e.upstream->getPeerAddress().describe();
    }
    RelayStateVisitor::SubscriptionInfo info{
        .ftn = e.ftn,
        .isPublish = e.isPublish,
        .subscribers = e.forwarder->subscriberCount(),
        .forwardingSubscribers = e.forwarder->numForwardingSubscribers(),
        .largest = e.forwarder->largest(),
        .totalGroupsReceived = e.forwarder->totalGroupsReceived(),
        .totalObjectsReceived = e.forwarder->totalObjectsReceived(),
        .sourceAddress = sourceAddr,
    };
    visitor.onSubscription(info);
  });
  visitor.onSubscriptionsEnd();

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

  if (cache_) {
    visitor.onCacheStats(
        cache_->totalCachedBytes(),
        cache_->getTrackStats(),
        MoqxCache::SteadyClock::now()
    );
  }
}

} // namespace openmoq::moqx
