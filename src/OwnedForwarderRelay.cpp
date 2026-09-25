/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "OwnedForwarderRelay.h"
#include "relay/CrossExecSubscriptionHandle.h"
#include "relay/InitialTrackState.h"
#include "relay/PublisherCrossExecFilter.h"
#include "relay/RelayDetail.h"
#include "relay/SubscriberCrossExecFilter.h"
#include "relay/TrackStatsFilter.h"
#include "relay/WeakRelayForwarderCallback.h"

using namespace moxygen;

namespace openmoq::moqx {

using namespace detail;

OwnedForwarderRelay::OwnedForwarderRelay(
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

std::shared_ptr<moxygen::Publisher> OwnedForwarderRelay::createPublisherFilter() {
  if (relayExec_) {
    return std::make_shared<PublisherCrossExecFilter>(relayExec_, shared_from_this());
  }
  return shared_from_this();
}

std::shared_ptr<moxygen::Subscriber> OwnedForwarderRelay::createSubscriberFilter() {
  if (relayExec_) {
    return std::make_shared<SubscriberCrossExecFilter>(relayExec_, shared_from_this());
  }
  return shared_from_this();
}

// Publisher::publish entry point for SingleThread/RelayExec modes (LF mode uses
// publishFromPublisherExec instead).
Subscriber::PublishResult OwnedForwarderRelay::publish(
    PublishRequest pub,
    std::shared_ptr<Publisher::SubscriptionHandle> handle
) {
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

void OwnedForwarderRelay::releasePublisherEntry(const FullTrackName& ftn) {
  // Clears handle + upstream; erases if no subscribers remain.
  auto kept = registry_.onPublisherTerminated(ftn);
  if (!kept) {
    XLOG(DBG1) << "Publisher terminated with no subscribers, cleaning up " << ftn;
  }
}

void OwnedForwarderRelay::wireForwarderCallback(const std::shared_ptr<MoQForwarder>& chainForwarder
) {
  // Weak ref breaks the registry → forwarder → callback → relay cycle.
  XCHECK(chainForwarder) << "publishWithSession: null chainForwarder in non-LF mode";
  chainForwarder->setCallback(std::make_shared<WeakRelayForwarderCallback>(weak_from_this()));
}

// Calls startPublish sync and fires the reply async. Returns false on synchronous failure.
bool OwnedForwarderRelay::addSubscriberAndPublish(
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

ForwarderRef OwnedForwarderRelay::makeForwarderRef(
    const std::shared_ptr<MoQForwarder>& forwarder,
    folly::Executor* /*publisherExec*/
) const {
  return ForwarderRef::owned(forwarder);
}

SubscriptionRegistry::FilterChainResult OwnedForwarderRelay::buildFilterChain(
    const FullTrackName& ftn,
    std::shared_ptr<MoQForwarder> forwarder
) {
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

folly::coro::Task<Publisher::SubscribeResult>
OwnedForwarderRelay::subscribe(SubscribeRequest subReq, std::shared_ptr<TrackConsumer> consumer) {
  return subscribeImpl(std::move(subReq), std::move(consumer));
}

folly::coro::Task<Publisher::SubscribeResult> OwnedForwarderRelay::subscribeImpl(
    SubscribeRequest subReq,
    std::shared_ptr<TrackConsumer> consumer
) {
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

std::optional<FetchError> OwnedForwarderRelay::resolveJoiningFetchOnRelay(
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

folly::coro::Task<std::optional<TrackStatusOk>> OwnedForwarderRelay::readLocalTrackStatus(
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

void OwnedForwarderRelay::evictOnOwner(
    const FullTrackName& ftn,
    const std::shared_ptr<MoQSession>& /*session*/,
    folly::Function<void(const std::shared_ptr<MoQForwarder>&)> evict
) {
  evict(registry_.getForwarderRef(ftn).getIfOwned());
}

std::shared_ptr<Publisher::SubscriptionHandle>
OwnedForwarderRelay::makePeerHandle(std::shared_ptr<MoQForwarder::Subscriber> subscriber) {
  // relayExec_ owns the forwarder, but the session calls unsubscribe() on its own io
  // thread, so the handle has to hop before it reaches subscribers_.
  if (relayExec_) {
    return std::make_shared<CrossExecSubscriptionHandle>(std::move(subscriber), relayExec_);
  }
  return subscriber;
}

} // namespace openmoq::moqx
