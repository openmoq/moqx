/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Originally from github.com/facebookexperimental/moxygen.
 * See deps/moxygen/LICENSE for the original license terms.
 *
 * Copyright (c) OpenMOQ contributors.
 */

#include "MoqxRelay.h"
#include "relay/CrossExecFilter.h"
#include "relay/CrossExecForwarderCallback.h"
#include "relay/LocalForwarderCallback.h"
#include "relay/NullConsumers.h"
#include "relay/PublisherCrossExecFilter.h"
#include "relay/SubscriberCrossExecFilter.h"
#include "relay/WeakRelayForwarderCallback.h"
#include <folly/container/F14Set.h>
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

std::shared_ptr<Subscriber::PublishNamespaceHandle> MoqxRelay::doPublishNamespace(
    PublishNamespace pubNs,
    std::shared_ptr<MoQSession> session,
    std::shared_ptr<Subscriber::PublishNamespaceCallback> callback,
    std::string peerID
) {
  XLOG(DBG1) << __func__ << " ns=" << pubNs.trackNamespace;
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
      // Bidi NAMESPACE is draft 16+ only; the handle is populated regardless of
      // version, so gate on it (matching doPublishNamespaceDone).
      auto maybeVersion = outSession->getNegotiatedVersion();
      if (maybeVersion.has_value() && getDraftMajorVersion(*maybeVersion) >= 16 &&
          info.namespacePublishHandle) {
        auto suffix = makeNamespaceSuffix(pubNs.trackNamespace, info.trackNamespacePrefix.size());
        info.namespacePublishHandle->namespaceMsg(suffix);
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
    if (outSession != session && (info.options == SubscribeNamespaceOptions::NAMESPACE ||
                                  info.options == SubscribeNamespaceOptions::BOTH)) {
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
    auto forwarder = registry_.onPublisherTerminated(ftn);
    if (!forwarder) {
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

// Constructs the publisher's local forwarder and installs its callback chain (Weak ->
// CrossExec -> LocalForwarder) on publisherExec, before the reply hops to relayExec_.
// tlForwarders_ must already be initialized.
std::shared_ptr<MoQForwarder> MoqxRelay::createPublisherForwarder(const PublishRequest& pub) {
  const auto& ftn = pub.fullTrackName;
  auto localPubFwd = std::make_shared<MoQForwarder>(ftn, pub.largest);
  localPubFwd->setExtensions(pub.extensions);

  // removeOnEmpty=false: the publisher's forwarder must survive subscriber churn, so
  // LocalForwarderCallback removes it from tlForwarders_ only when the source ends.
  auto relayAdapter = std::make_shared<WeakRelayForwarderCallback>(weak_from_this());
  auto crossExec = std::make_shared<CrossExecForwarderCallback>(
      relayExec_,
      localPubFwd,
      std::move(relayAdapter)
  );
  localPubFwd->setCallback(std::make_shared<LocalForwarderCallback>(
      tlForwarders_.get(),
      ftn,
      std::move(crossExec),
      /*removeOnEmpty=*/false
  ));

  return localPubFwd;
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

  if (!tlForwarders_.get()) {
    tlForwarders_.reset(new LocalForwarderRegistry());
  }

  auto localPubFwd = createPublisherForwarder(pub);

  // The publisher's forwarder is authoritative — claim the slot, displacing any
  // stale subscribe-path local forwarder so same-thread subscribers reuse THIS
  // forwarder via the fast path.
  tlForwarders_->set(pub.fullTrackName, localPubFwd);

  // crossExecFilter is a channel subscriber for the relay exec
  // regulsterPublishOnRelay exec completes wiring the chain (topNFilter → terminationFilter →
  // cache).
  auto crossExecFilter = std::make_shared<CrossExecFilter>(relayExec_, nullptr);
  // forward=true + passive=true: internal relay chain observes all objects but
  // does not count as a real forwarding subscriber.
  localPubFwd
      ->addChannelSubscriber(relayExec_, /*forward=*/true, crossExecFilter, /*passive=*/true);

  auto reply = folly::coro::co_invoke(
      [exec = relayExec_,
       relay = shared_from_this(),
       pub = std::move(pub),
       handle = std::move(handle),
       session = std::move(session),
       localPubFwd,
       crossExecFilter]() mutable -> folly::coro::Task<folly::Expected<PublishOk, PublishError>> {
        co_return co_await folly::coro::co_withExecutor(
            folly::getKeepAliveToken(exec),
            relay->registerPublishOnRelayExec(
                std::move(pub),
                std::move(handle),
                std::move(session),
                std::move(localPubFwd),
                std::move(crossExecFilter)
            )
        );
      }
  );

  auto consumer = std::static_pointer_cast<TrackConsumer>(std::move(localPubFwd));
  return PublishConsumerAndReplyTask{std::move(consumer), std::move(reply)};
}

// Runs on relayExec_. Registers publisherFwd in the registry and wires
// relayChainFilter to the topN filter.
folly::coro::Task<folly::Expected<PublishOk, PublishError>> MoqxRelay::registerPublishOnRelayExec(
    PublishRequest pub,
    std::shared_ptr<Publisher::SubscriptionHandle> handle,
    std::shared_ptr<MoQSession> session,
    std::shared_ptr<MoQForwarder> publisherFwd,
    std::shared_ptr<CrossExecFilter> relayChainFilter
) {
  auto ftn = pub.fullTrackName;
  auto setup =
      publishWithSession(std::move(pub), std::move(handle), std::move(session), publisherFwd);
  if (setup.hasError()) {
    co_return folly::makeUnexpected(setup.error());
  }

  auto topNView = registry_.getTopNView(ftn);
  XCHECK(topNView && topNView->topNFilter)
      << "registerPublishOnRelayExec: topNFilter always present in MT mode";
  relayChainFilter->setDownstream(topNView->topNFilter);

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
  auto setup = publishWithSession(std::move(pub), std::move(handle), std::move(session));
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
    std::shared_ptr<MoQForwarder> forwarder
) {
  // Handle duplicate publisher at relay level before registering in the tree.
  if (!forwarder) {
    forwarder = std::make_shared<MoQForwarder>(pub.fullTrackName, pub.largest);
    forwarder->setExtensions(pub.extensions);
  }

  auto publisherWrapped = maybeWrapPublisher(relayExec_, session);
  auto publishEntry = registry_.createFromPublish(
      pub.fullTrackName,
      forwarder,
      session,
      std::move(publisherWrapped),
      pub.requestID,
      std::move(handle),
      [&](std::shared_ptr<MoQForwarder> f) { return buildFilterChain(pub.fullTrackName, f); }
  );

  if (publishEntry.evicted) {
    // A new publisher replaced a live one. createFromPublish already erased the old registry entry
    // and handed us its last ref, so the onEmpty that publishDone may re-enter (via
    // drainSubscriber) can't destroy the forwarder mid-forEachSubscriber.
    XLOG(DBG1) << "New publisher for existing subscription";
    auto& evicted = *publishEntry.evicted;
    // Null handle => previous publisher already terminated and onPublishDone() tore it down; skip.
    if (evicted.handle) {
      // unsubscribe mutates the old publisher's session inline, so hop to its exec.
      runOnSessionExec(relayExec_, evicted.publisherExec, [h = evicted.handle] {
        h->unsubscribe();
      });
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

  switch (mode()) {
  case Mode::SingleThread:
  case Mode::RelayExec:
    // Weak ref breaks the registry → forwarder → callback → relay cycle.
    forwarder->setCallback(std::make_shared<WeakRelayForwarderCallback>(weak_from_this()));
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
      auto* publisherExec = relayExec_ ? session->getExecutor() : nullptr;
      if (!addSubscriberAndPublish(
              outSession,
              forwarder,
              info.forward,
              /*pinned=*/true,
              publisherExec
          )) {
        XLOG(ERR) << "addSubscriberAndPublish failed for " << forwarder->fullTrackName();
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
      auto* publisherExec = relayExec_ ? session->getExecutor() : nullptr;
      if (!addSubscriberAndPublish(
              outSession,
              forwarder,
              info.forward,
              /*pinned=*/true,
              publisherExec
          )) {
        XLOG(ERR) << "addSubscriberAndPublish failed for " << forwarder->fullTrackName();
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
  subscriber->trackConsumer = std::move(pub->consumer);
  return PreparedPublish{std::move(subscriber), std::move(pub->reply)};
}

// In relay+local-forwarder mode: dispatches addSubscriberAndPublishViaLocalForwarder
// to subscriberExec. Otherwise: calls startPublish sync and fires reply async.
// Returns false on synchronous failure.
bool MoqxRelay::addSubscriberAndPublish(
    std::shared_ptr<MoQSession> subscriberSession,
    std::shared_ptr<MoQForwarder> forwarder,
    bool forward,
    bool pinned,
    folly::Executor* publisherExec
) {
  if (mode() == Mode::LocalForwarder) {
    XCHECK(publisherExec
    ) << "addSubscriberAndPublish: publisherExec required in local-forwarder mode";
    co_withExecutor(
        folly::getKeepAliveToken(subscriberSession->getExecutor()),
        addSubscriberAndPublishViaLocalForwarder(
            subscriberSession,
            forwarder,
            publisherExec,
            forward,
            pinned
        )
    )
        .start();
    return true;
  }
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

// Tears down an aborted local-forwarder setup: drops the channel sub(s) on the publisher (hopping
// to publisherExec; the relayExec entry only when relayExec is non-null and drains localFwd via
// publishDone if given.
void teardownLocalForwarderOnFailure(
    folly::Executor* publisherExec,
    std::shared_ptr<MoQForwarder> publisherFwd,
    folly::Executor* subscriberExec,
    folly::Executor* relayExec,
    const std::shared_ptr<MoQForwarder>& localFwd = nullptr,
    std::string publishDoneReason = {}
) {
  if (publisherFwd && publisherExec) {
    folly::via(
        publisherExec,
        [pf = std::move(publisherFwd), ex = subscriberExec, re = relayExec]() noexcept {
          pf->removeChannelSubscriberByExec(ex);
          if (re) {
            pf->removeChannelSubscriberByExec(re);
          }
        }
    );
  }
  if (localFwd) {
    localFwd->publishDone(PublishDone{
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
//     CrossExecForwarderCallback(relayExec_, publisherFwd,
//       WeakRelayForwarderCallback(relay))
//
//   [publisherExec] CrossExecForwarderCallback: captures ftn by value,
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
//         CrossExecForwarderCallback(publisherExec, localFwd,
//           ChannelForwarderCallback(publisherFwd, subscriberExec, publisherExec)))
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
//   CrossExecForwarderCallback holds weak_ptr<forwarder> to avoid a permanent
//   ownership cycle; it locks eagerly on the calling thread (where the forwarder
//   is alive) and moves the shared_ptr into the lambda to keep it alive across
//   the executor hop.

// Captures forwardChanged/newGroupRequested/onEmpty during setup (getOrCreate→setCallback window)
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
class ChannelForwarderCallback : public moxygen::MoQForwarder::Callback {
public:
  ChannelForwarderCallback(
      std::weak_ptr<moxygen::MoQForwarder> weakPublisher,
      folly::Executor* subscriberExec,
      folly::Executor* publisherExec
  )
      : weakPublisher_(std::move(weakPublisher)), subscriberExec_(subscriberExec),
        publisherExec_(publisherExec) {}

  // Called on publisherExec immediately after addChannelSubscriber returns.
  void setHandle(std::shared_ptr<moxygen::Publisher::SubscriptionHandle> h) {
    handle_ = std::move(h);
  }

  // Holds the last filter ref so onEmpty can defer its destruction to subscriberExec_,
  // letting in-flight this-capturing lambdas there run first (FIFO).
  void setFilter(std::shared_ptr<CrossExecFilter> filter) { crossExecFilter_ = std::move(filter); }

  void onEmpty(moxygen::MoQForwarder* /*localFwd*/) override {
    auto publisher = weakPublisher_.lock();
    if (publisher) {
      publisher->removeChannelSubscriberByExec(subscriberExec_);
    }
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

  void forwardChanged(moxygen::MoQForwarder*, bool forward) override {
    if (!handle_) {
      return;
    }
    launchUpdate(publisherExec_, doSubscribeUpdate(handle_, forward));
  }

  void newGroupRequested(moxygen::MoQForwarder*, uint64_t group) override {
    if (!handle_) {
      return;
    }
    launchUpdate(publisherExec_, doNewGroupRequestUpdate(handle_, group));
  }

private:
  std::weak_ptr<moxygen::MoQForwarder> weakPublisher_;
  folly::Executor* subscriberExec_;
  folly::Executor* publisherExec_;
  std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle_;
  std::shared_ptr<CrossExecFilter> crossExecFilter_;
};

// Builds the local->publisher callback chain (Channel -> CrossExec -> LocalForwarder).
// channelCb is wired with the channel handle/filter later, on publisherExec.
struct LocalToPublisherCallbacks {
  std::shared_ptr<ChannelForwarderCallback> channelCb;
  std::shared_ptr<moxygen::MoQForwarder::Callback> finalCallback;
};
LocalToPublisherCallbacks buildLocalToPublisherCallbacks(
    openmoq::moqx::LocalForwarderRegistry* localReg,
    moxygen::FullTrackName ftn,
    const std::shared_ptr<moxygen::MoQForwarder>& localFwd,
    const std::shared_ptr<moxygen::MoQForwarder>& publisherFwd,
    folly::Executor* publisherExec,
    folly::Executor* subscriberExec
) {
  auto channelCb =
      std::make_shared<ChannelForwarderCallback>(publisherFwd, subscriberExec, publisherExec);
  auto crossExecCb =
      std::make_shared<CrossExecForwarderCallback>(publisherExec, localFwd, channelCb);
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

// Wires localFwd to publisherFwd as a channel subscriber, hopping to publisherExec.
// Returns the finalCallback to install on localFwd. Used by the publish LF path.
folly::coro::Task<std::shared_ptr<moxygen::MoQForwarder::Callback>> addLocalForwarderToPublisher(
    openmoq::moqx::LocalForwarderRegistry* localReg,
    moxygen::FullTrackName ftn,
    std::shared_ptr<moxygen::MoQForwarder> localFwd,
    std::shared_ptr<moxygen::MoQForwarder> publisherFwd,
    folly::Executor* publisherExec,
    folly::Executor* subscriberExec,
    std::shared_ptr<CrossExecFilter> crossExecFilter,
    bool forward
) {
  auto cbs = buildLocalToPublisherCallbacks(
      localReg,
      std::move(ftn),
      localFwd,
      publisherFwd,
      publisherExec,
      subscriberExec
  );
  co_await folly::coro::co_withExecutor(
      folly::getKeepAliveToken(publisherExec),
      [&]() -> folly::coro::Task<void> {
        installChannelSubscriber(
            *cbs.channelCb,
            *publisherFwd,
            subscriberExec,
            forward,
            crossExecFilter
        );
        co_return;
      }()
  );
  co_return cbs.finalCallback;
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

// Runs on subscriberExec. Gets or creates the thread-local forwarder for ftn, wires
// it to publisherFwd as a channel subscriber (isNew path), and awaits the publish reply.
folly::coro::Task<void> MoqxRelay::addSubscriberAndPublishViaLocalForwarder(
    std::shared_ptr<MoQSession> subscriberSession,
    std::shared_ptr<MoQForwarder> publisherFwd,
    folly::Executor* publisherExec,
    bool forward,
    bool pinned
) {
  auto* subscriberExec = subscriberSession->getExecutor();
  const auto& ftn = publisherFwd->fullTrackName();

  // Fast path: local forwarder already exists on this thread.
  if (auto* localReg = tlForwarders_.get()) {
    if (auto localFwd = localReg->get(ftn)) {
      auto p = startPublish(subscriberSession, localFwd, forward, pinned, nullptr);
      if (p) {
        co_await awaitPublishReply(localFwd, std::move(p->subscriber), std::move(p->reply));
      }
      co_return;
    }
  }

  // Capture largest/extensions on publisherExec; reading them on subscriberExec would
  // race the publisher advancing largest_.
  std::optional<AbsoluteLocation> seedLargest;
  Extensions seedExtensions;
  co_await folly::coro::co_withExecutor(
      folly::getKeepAliveToken(publisherExec),
      [&]() -> folly::coro::Task<void> {
        seedLargest = publisherFwd->largest();
        seedExtensions = publisherFwd->extensions();
        co_return;
      }()
  );

  auto [localFwd, isNew, localReg] = acquireLocalForwarder(ftn, [&] {
    auto fwd = std::make_shared<MoQForwarder>(ftn, seedLargest);
    fwd->setExtensions(seedExtensions);
    return fwd;
  });

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
  auto crossExecFilter = std::make_shared<CrossExecFilter>(subscriberExec, localFwd);
  bool hasForwardingSub = (localFwd->numForwardingSubscribers() > 0);

  auto finalCallback = co_await addLocalForwarderToPublisher(
      localReg,
      ftn,
      localFwd,
      publisherFwd,
      publisherExec,
      subscriberExec,
      crossExecFilter,
      hasForwardingSub
  );

  // Natural unwind: back on subscriberExec.

  if (pendingCb->sawOnEmpty_) {
    teardownLocalForwarderOnFailure(
        publisherExec,
        publisherFwd,
        subscriberExec,
        /*relayExec=*/nullptr,
        localFwd,
        "all subscribers cancelled during setup"
    );
    co_return;
  }

  replayPendingFowarderEvents(localFwd.get(), finalCallback, *pendingCb, hasForwardingSub);
  co_await awaitPublishReply(localFwd, std::move(p->subscriber), std::move(p->reply));
}

// Slow-path local-forwarder bootstrap shared by the publish and subscribe LF paths:
// ensures the thread-local registry, then getOrCreates the forwarder for ftn. Callers
// handle the fast path and install the PendingForwarderCallback (timing differs).
MoqxRelay::LocalForwarderBootstrap MoqxRelay::acquireLocalForwarder(
    const FullTrackName& ftn,
    folly::FunctionRef<std::shared_ptr<MoQForwarder>()> factory
) {
  if (!tlForwarders_.get()) {
    tlForwarders_.reset(new LocalForwarderRegistry());
  }
  auto* localReg = tlForwarders_.get();
  auto [localFwd, isNew] = localReg->getOrCreate(ftn, factory);
  return {std::move(localFwd), isNew, localReg};
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
  if (mode() == Mode::LocalForwarder) {
    // Multi-iothread with local forwarders: publisher writes directly to forwarder on
    // publisherExec. relayChainFilter (added by publish()) fans off to
    // topNFilter/termination/cache.
    std::shared_ptr<TrackConsumer> chainEnd =
        cache_ ? cache_->makePassiveConsumer(ftn) : std::make_shared<moxygen::NullTrackConsumer>();
    auto terminationFilter =
        std::make_shared<TerminationFilter>(shared_from_this(), ftn, std::move(chainEnd));
    auto topNFilter = std::make_shared<TopNFilter>(
        ftn,
        std::static_pointer_cast<TrackConsumer>(terminationFilter)
    );
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
        /*passive=*/true
    );
  }
  auto terminationFilter = std::make_shared<TerminationFilter>(
      shared_from_this(),
      ftn,
      std::static_pointer_cast<TrackConsumer>(forwarder)
  );
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
    // maybeWrapPublisher runs the call on the peer session's executor and wraps
    // the returned handle so its teardown hops there too (no token: reciprocal).
    auto recipResult = co_await maybeWrapPublisher(relayExec_, session)
                           ->subscribeNamespace(makePeerSubNs(), handle);
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
        if (node->publisherSession() && node->publisherSession() != session &&
            (incomingPeerID.empty() || node->publisherPeerID() != incomingPeerID)) {
          if (getDraftMajorVersion(*maybeNegotiatedVersion) >= 16) {
            if (subNs.options == SubscribeNamespaceOptions::NAMESPACE ||
                subNs.options == SubscribeNamespaceOptions::BOTH) {
              // Compute the suffix: prefix minus subNs.trackNamespacePrefix
              auto suffix = makeNamespaceSuffix(prefix, subNs.trackNamespacePrefix.size());
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
              auto* publisherExec = relayExec_ ? publishSession->getExecutor() : nullptr;
              if (!addSubscriberAndPublish(
                      session,
                      forwarder,
                      subNs.forward,
                      /*pinned=*/true,
                      publisherExec
                  )) {
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
          /*trackFilter=*/std::nullopt
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
            auto forwarder = registry_.getForwarder(ftn);
            if (!forwarder) {
              return;
            }
            auto* publisherExec = relayExec_ ? publishSession->getExecutor() : nullptr;
            if (!addSubscriberAndPublish(
                    session,
                    forwarder,
                    subTracks.forward,
                    /*pinned=*/true,
                    publisherExec
                )) {
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
  if (ok.largest) {
    publisherFwd->updateLargest(ok.largest->group, ok.largest->object);
  }
  publisherFwd->setExtensions(ok.extensions);
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
// returned PublisherAttachment (handles upstreamOk).
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
  attach.publisherFwd = sr.publisherForwarder;
  attach.publisherExec = sr.publisherExec;
  if (!attach.publisherFwd || !attach.publisherExec) {
    co_return attach;
  }
  attach.ownsRelayChain = sr.firstSetup.has_value();

  // Single publisherExec sortie for all publisherFwd mutation: the local channel sub and,
  // for the first subscriber, the passive relay chain + upstream subscribe. Merging the
  // two installs drops a relayExec_ round-trip.
  auto cbs = buildLocalToPublisherCallbacks(
      localReg,
      ftn,
      localFwd,
      attach.publisherFwd,
      attach.publisherExec,
      subscriberExec
  );
  attach.finalCallback = cbs.finalCallback;

  std::shared_ptr<CrossExecFilter> relayChainFilter;
  std::optional<folly::Expected<UpstreamOk, SubscribeError>> upstreamResult;

  co_await folly::coro::co_withExecutor(
      folly::getKeepAliveToken(attach.publisherExec),
      [&]() -> folly::coro::Task<void> {
        installChannelSubscriber(
            *cbs.channelCb,
            *attach.publisherFwd,
            subscriberExec,
            forward,
            crossExecFilter
        );

        if (sr.firstSetup) {
          auto& setup = *sr.firstSetup;
          // Passive relay chain (top-N/termination/cache): forward=true so it observes every
          // object, passive=true so it doesn't count as a forwarding subscriber or in the
          // onEmpty quorum (the publisher's onEmpty still fires when the last real sub leaves).
          relayChainFilter = std::make_shared<CrossExecFilter>(relayExec_, nullptr);
          attach.publisherFwd->addChannelSubscriber(
              relayExec_,
              /*forward=*/true,
              relayChainFilter,
              /*passive=*/true
          );
          setup.upstreamSubReq.forward = forward;
          upstreamResult = co_await subscribeUpstreamAndApplyOk(
              setup.upstreamSession,
              std::move(setup.upstreamSubReq),
              std::move(setup.upstreamConsumer),
              attach.publisherFwd,
              setup.clientRequestID
          );
        }
      }()
  );
  // Back on relayExec_.

  if (!sr.firstSetup) {
    co_return attach; // subsequent subscriber: wired to the live publisher, done
  }

  if (upstreamResult->hasError()) {
    // Upstream subscribe failed: drop the channel sub(s) we installed (the relay-exec
    // sub too, since firstSetup owns the relay chain). localFwd is drained by the tail.
    teardownLocalForwarderOnFailure(
        attach.publisherExec,
        attach.publisherFwd,
        subscriberExec,
        relayChainFilter ? relayExec_ : nullptr
    );
    attach.error = folly::makeUnexpected(std::move(upstreamResult->error()));
    co_return attach;
  }
  auto upstreamOk = std::move(upstreamResult->value());

  // Wire the relay chain to topNFilter before pending.complete() so buffered objects
  // see the filter.
  if (relayChainFilter) {
    auto topNView = registry_.getTopNView(ftn);
    if (topNView && topNView->topNFilter) {
      relayChainFilter->setDownstream(topNView->topNFilter);
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
  attach.upstreamOk = std::move(upstreamOk);
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

  auto firstOrSubsequent = registry_.getOrCreateFromSubscribe(
      ftn,
      shared_from_this(),
      [this, &ftn](std::shared_ptr<MoQForwarder> f) { return buildFilterChain(ftn, std::move(f)); }
  );

  if (auto* first = std::get_if<SubscriptionRegistry::FirstSubscriber>(&firstOrSubsequent)) {
    auto upstreamSession = namespaceTree_.findPublisherSession(ftn.trackNamespace);
    if (!upstreamSession) {
      co_return StatefulSubscribeResult{
          nullptr,
          nullptr,
          folly::makeUnexpected(SubscribeError{
              subReq.requestID,
              SubscribeErrorCode::DOES_NOT_EXIST,
              "no such namespace or track"
          }),
          std::nullopt
      };
    }

    const auto clientRequestID = subReq.requestID;
    // forward updated to its real value in attachNewLocalForwarderOnRelayExec.
    SubscribeRequest upstreamSubReq = makeUpstreamSubReq(subReq, /*forward=*/false);

    // first->consumer is the publisher forwarder (lives on publisherExec == upstreamSession's
    // executor). No cross-exec wrapping — upstream delivers on that executor directly.
    auto upstreamConsumer = first->consumer;
    StatefulSubscribeResult
        result{first->forwarder, upstreamSession->getExecutor(), std::nullopt, std::nullopt};
    result.firstSetup.emplace(StatefulSubscribeResult::FirstSubscriberSetup{
        upstreamSession,
        std::move(upstreamSubReq),
        std::move(upstreamConsumer),
        std::move(first->pending),
        clientRequestID
    });
    co_return result;

  } else {
    auto sub = co_await std::get<folly::coro::Task<SubscriptionRegistry::SubsequentSubscriber>>(
        std::move(firstOrSubsequent)
    );
    auto upstreamView = registry_.getUpstreamView(ftn);
    auto* publisherExec = upstreamView ? upstreamView->publisherExec : nullptr;
    co_return StatefulSubscribeResult{sub.forwarder, publisherExec, std::nullopt, std::nullopt};
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

  // getOrCreate before the relay hop: serializes same-iothread races. isNew=false means
  // the forwarder already exists (or a setup is in progress) on this thread — just attach.
  auto [localFwd, isNew, localReg] =
      acquireLocalForwarder(ftn, [&] { return std::make_shared<MoQForwarder>(ftn); });

  if (!isNew) {
    if (auto err = checkRangeNotInPast(*localFwd, subReq)) {
      co_return folly::makeUnexpected(std::move(*err));
    }
    co_return attachSubscriber(*localFwd, std::move(session), subReq, std::move(consumer));
  }

  // isNew=true: this thread owns setup. Install PendingForwarderCallback first so
  // forwardChanged/newGroupRequested/onEmpty events during setup are captured for replay.
  auto pendingCb = std::make_shared<PendingForwarderCallback>(localReg, ftn);
  localFwd->setCallback(pendingCb);

  // deepCopyPayload=true (default): each subscriber thread owns its IOBuf chain,
  // avoiding cross-thread contention on the shared atomic refcount.
  auto crossExecFilter = std::make_shared<CrossExecFilter>(subscriberExec, localFwd);

  // addSubscriber before the relay hop: numForwardingSubscribers() must be correct
  // when addChannelSubscriber runs on publisherExec, so forward flag is right from the start.
  auto sub = localFwd->addSubscriber(session, subReq, std::move(consumer));
  if (!sub) {
    localReg->remove(ftn, localFwd.get());
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
        attach.publisherExec,
        attach.publisherFwd,
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
    localFwd->publishDone(PublishDone{
        RequestID(0),
        PublishDoneStatusCode::INTERNAL_ERROR,
        0,
        attach.error->error().reasonPhrase
    });
    localReg->remove(ftn, localFwd.get());
    co_return std::move(*attach.error);
  }

  if (attach.upstreamOk) {
    localFwd->setExtensions(attach.upstreamOk->extensions);
    if (attach.upstreamOk->largest) {
      localFwd->updateLargest(
          attach.upstreamOk->largest->group,
          attach.upstreamOk->largest->object
      );
    }
  }
  replayPendingFowarderEvents(localFwd.get(), attach.finalCallback, *pendingCb, forward);
  localFwd->tryProcessNewGroupRequest(subReq.params);
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

  auto firstOrSubsequent = registry_.getOrCreateFromSubscribe(
      ftn,
      shared_from_this(),
      [this, &ftn](std::shared_ptr<MoQForwarder> f) { return buildFilterChain(ftn, std::move(f)); }
  );

  if (auto* first = std::get_if<SubscriptionRegistry::FirstSubscriber>(&firstOrSubsequent)) {
    auto upstreamSession = namespaceTree_.findPublisherSession(ftn.trackNamespace);
    if (!upstreamSession) {
      co_return folly::makeUnexpected(SubscribeError(
          {subReq.requestID, SubscribeErrorCode::DOES_NOT_EXIST, "no such namespace or track"}
      ));
    } // pending destructor fires on early return above
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
    if (ok.largest) {
      subscriber->updateLargest(*ok.largest);
    }
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
    // sub.forwarder is valid: promise was fulfilled by first subscriber
    if (auto err = checkRangeNotInPast(*sub.forwarder, subReq)) {
      co_return folly::makeUnexpected(std::move(*err));
    }
    co_return attachSubscriber(*sub.forwarder, std::move(session), subReq, std::move(consumer));
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
  auto localFwd = localReg ? localReg->get(req.fullTrackName) : nullptr;
  if (!localFwd || localFwd->numForwardingSubscribers() == 0) {
    return std::nullopt;
  }
  // A forwarding local subscriber implies the upstream sub is live.
  return Publisher::TrackStatusResult(buildTrackStatusOk(*localFwd, /*hasHandle=*/true, req));
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
  if (joining) {
    auto fetchView = registry_.getFetchView(fetch.fullTrackName);
    if (!fetchView) {
      XLOG(ERR) << "No subscription for joining fetch";
      co_return folly::makeUnexpected(FetchError(
          {fetch.requestID, FetchErrorCode::DOES_NOT_EXIST, "No subscription for joining fetch"}
      ));
    } else if (fetchView->isReady) {
      auto res = fetchView->forwarder->resolveJoiningFetch(session, *joining);
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
  auto forwarder = localReg ? localReg->get(req.fullTrackName) : nullptr;
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
  } else if (upstreamView && upstreamView->forwarder &&
             upstreamView->forwarder->numForwardingSubscribers() > 0) {
    // Non-LF: relayExec_ (or the single thread) owns the sole forwarder; read it inline.
    trackStatusOk =
        buildTrackStatusOk(*upstreamView->forwarder, (bool)upstreamView->handle, trackStatus);
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

  if (!session) {
    XLOG(ERR) << "onTrackSelected: null session for " << ftn;
    return;
  }

  auto trackForwarder = registry_.getForwarder(ftn);
  if (!trackForwarder) {
    XLOG(DBG4) << "onTrackSelected: no subscription/forwarder for " << ftn;
    return;
  }

  auto upstreamView = registry_.getUpstreamView(ftn);
  XCHECK(!relayExec_ || (upstreamView && upstreamView->publisherExec))
      << "onTrackSelected: relayExec set but no publisherExec for " << ftn;
  auto* publisherExec = relayExec_ ? upstreamView->publisherExec : nullptr;
  // TRACK_FILTER subscribers are unpinned so onTrackEvicted can remove them.
  addSubscriberAndPublish(session, trackForwarder, forward, /*pinned=*/false, publisherExec);
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
      evict(localReg ? localReg->get(ftn) : nullptr);
    });
    return;
  }
  evict(registry_.getForwarder(ftn));
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
