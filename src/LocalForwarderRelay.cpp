/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LocalForwarderRelay.h"
#include "relay/CrossExecFilter.h"
#include "relay/CrossExecForwarderCallback.h"
#include "relay/LocalForwarderCallback.h"
#include "relay/NullConsumers.h"
#include "relay/PublisherCrossExecFilter.h"
#include "relay/RelayDetail.h"
#include "relay/SubscriberCrossExecFilter.h"
#include "relay/TrackEventCallback.h"
#include "relay/TrackStatsFilter.h"
#include "relay/WeakRelayForwarderCallback.h"
#include <folly/futures/Future.h>

using namespace moxygen;

namespace openmoq::moqx {

using namespace detail;

LocalForwarderRelay::LocalForwarderRelay(
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
      ) {
  XCHECK(relayExec_) << "LocalForwarderRelay requires a relay executor";
}

// === LocalSubscribeFilter and LocalPublishFilter ===

// LF-mode publish handler: overrides subscribe() to run subscribeFromSubscriberExec
// on the subscriber's executor (no relayExec_ hop). Other Publisher methods fall
// through to PublisherCrossExecFilter.
class LocalForwarderRelay::LocalSubscribeFilter final : public PublisherCrossExecFilter {
public:
  LocalSubscribeFilter(folly::Executor* relayExec, std::shared_ptr<LocalForwarderRelay> relay)
      : PublisherCrossExecFilter(relayExec, relay), relay_(std::move(relay)) {}

  folly::coro::Task<SubscribeResult> subscribe(
      moxygen::SubscribeRequest subReq,
      std::shared_ptr<moxygen::TrackConsumer> consumer
  ) override {
    auto session = moxygen::MoQSession::getRequestSession();
    if (subReq.fullTrackName.trackNamespace.empty() && !MoqxRelay::emptyNamespaceAllowed(session)) {
      co_return folly::makeUnexpected(moxygen::SubscribeError{
          subReq.requestID,
          moxygen::SubscribeErrorCode::DOES_NOT_EXIST,
          "namespace required"
      });
    }
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
  std::shared_ptr<LocalForwarderRelay> relay_;
};

std::shared_ptr<moxygen::Publisher> LocalForwarderRelay::createPublisherFilter() {
  return std::make_shared<LocalSubscribeFilter>(relayExec_, sharedSelf());
}

// LF-mode subscribe handler: overrides publish() to create the publisher's local
// forwarder on its own executor (no cross-exec hop for data). Other Subscriber methods
// fall through to SubscriberCrossExecFilter.
class LocalForwarderRelay::LocalPublishFilter final : public SubscriberCrossExecFilter {
public:
  LocalPublishFilter(folly::Executor* relayExec, std::shared_ptr<LocalForwarderRelay> relay)
      : SubscriberCrossExecFilter(relayExec, relay), relay_(std::move(relay)) {}

  PublishResult publish(
      moxygen::PublishRequest pub,
      std::shared_ptr<moxygen::SubscriptionHandle> handle
  ) override {
    auto session = moxygen::MoQSession::getRequestSession();
    return relay_->publishFromPublisherExec(std::move(pub), std::move(handle), std::move(session));
  }

private:
  std::shared_ptr<LocalForwarderRelay> relay_;
};

std::shared_ptr<moxygen::Subscriber> LocalForwarderRelay::createSubscriberFilter() {
  return std::make_shared<LocalPublishFilter>(relayExec_, sharedSelf());
}

folly::coro::Task<Publisher::SubscribeResult>
LocalForwarderRelay::subscribe(SubscribeRequest, std::shared_ptr<TrackConsumer>) {
  XLOG(FATAL) << "subscribe() bypassed by LocalSubscribeFilter in LF mode";
}

Subscriber::PublishResult
LocalForwarderRelay::publish(PublishRequest, std::shared_ptr<Publisher::SubscriptionHandle>) {
  XLOG(FATAL) << "publish() bypassed by LocalPublishFilter in LF mode";
}

void LocalForwarderRelay::releasePublisherEntry(const FullTrackName& ftn) {
  // The publisher forwarder is [Pub]-owned — never inspect it from relayExec_.
  // The entry only indexes a live publisher, so drop it outright.
  registry_.remove(ftn);
}

// Chain and entry claim are one call: LocalForwarderCallback vacates the entry when the
// source ends, so a chain over a forwarder that does not hold it has nothing to remove.
LocalForwarderRegistry::ParkResult LocalForwarderRelay::installPublisherForwarder(
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
Subscriber::PublishResult LocalForwarderRelay::publishFromPublisherExec(
    PublishRequest pub,
    std::shared_ptr<Publisher::SubscriptionHandle> handle,
    std::shared_ptr<MoQSession> session
) {
  if (auto err = validatePublishNamespace(
          pub.fullTrackName,
          pub.requestID,
          emptyNamespaceAllowed(session)
      )) {
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
       relay = sharedSelf(),
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
folly::coro::Task<folly::Expected<PublishOk, PublishError>>
LocalForwarderRelay::registerPublishOnRelayExec(
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

bool LocalForwarderRelay::addSubscriberAndPublish(
    std::shared_ptr<MoQSession> subscriberSession,
    const ForwarderRef& publisherRef,
    bool forward,
    bool pinned
) {
  XCHECK(publisherRef) << "addSubscriberAndPublish: empty forwarder ref";
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

folly::coro::Task<std::optional<LocalForwarderRelay::ResolvedPublisher>>
LocalForwarderRelay::resolvePublisherOnItsExec(TrackRef track) {
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
folly::coro::Task<void> LocalForwarderRelay::addSubscriberAndPublishViaLocalForwarder(
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

ForwarderRef LocalForwarderRelay::makeForwarderRef(
    const std::shared_ptr<MoQForwarder>& forwarder,
    folly::Executor* publisherExec
) const {
  return ForwarderRef::remote(forwarder, folly::getKeepAliveToken(publisherExec));
}

LocalForwarderRegistry& LocalForwarderRelay::localRegistry() {
  if (!tlForwarders_.get()) {
    tlForwarders_.reset(new LocalForwarderRegistry());
  }
  return *tlForwarders_;
}

// Slow-path local-forwarder bootstrap shared by the publish and subscribe LF paths.
// Callers handle the fast path and install the PendingForwarderCallback themselves,
// because the timing differs.
LocalForwarderRelay::LocalForwarderBootstrap LocalForwarderRelay::acquireLocalForwarder(
    const FullTrackName& ftn,
    const InitialTrackState& initial
) {
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

SubscriptionRegistry::FilterChainResult LocalForwarderRelay::buildFilterChain(
    const FullTrackName& ftn,
    std::shared_ptr<MoQForwarder> forwarder
) {
  // Multi-iothread with local forwarders: publisher writes directly to forwarder on
  // publisherExec. relayChainFilter (added by publish()) fans off to
  // topNFilter/termination/cache.
  std::shared_ptr<TrackConsumer> chainEnd =
      cache_ ? cache_->makePassiveConsumer(ftn) : std::make_shared<moxygen::NullTrackConsumer>();
  auto chain = makeIngestChain(ftn, std::move(chainEnd));
  return SubscriptionRegistry::FilterChainResult{
      .consumer = std::static_pointer_cast<TrackConsumer>(forwarder),
      .topNFilter = std::move(chain.topNFilter),
      .chainHead = std::move(chain.chainHead),
      .ingest = std::move(chain.ingest)
  };
}

// Runs on relayExec_. Registers the subscribe and wires the local forwarder to the
// publisher; if it's a new subscription,also installs the passive relay chain, issues the
// upstream subscribe, and completes the setup. The subscriberExec tail reads the
// returned PublisherAttachment.
folly::coro::Task<LocalForwarderRelay::PublisherAttachment>
LocalForwarderRelay::attachNewLocalForwarderOnRelayExec(
    const SubscribeRequest& subReq,
    LocalForwarderRegistry* subscriberReg,
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
  if (sr.firstSetup) {
    // Built with its downstream: a CrossExecFilter that reaches the forwarder without one
    // fails the first object, and the forwarder then drops it for the life of the track.
    auto topNView = registry_.getTopNView(ftn);
    XCHECK(topNView && topNView->chainHead) << "relay chain missing for " << ftn;
    relayChainFilter = CrossExecFilter::create(relayExec_, topNView->chainHead);
  }
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
          if (localRegistry().getIfReady(ftn)) {
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
            subscriberReg,
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
folly::coro::Task<LocalForwarderRelay::StatefulSubscribeResult>
LocalForwarderRelay::joinOrPrepareUpstreamSubscription(SubscribeRequest subReq) {
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
    SubscribeRequest upstreamSubReq =
        makeUpstreamSubReq(subReq, /*forward=*/false, upstreamSession);

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
folly::coro::Task<Publisher::SubscribeResult> LocalForwarderRelay::subscribeFromSubscriberExec(
    SubscribeRequest subReq,
    std::shared_ptr<TrackConsumer> consumer,
    std::shared_ptr<MoQSession> session,
    folly::Executor* subscriberExec
) {
  const auto& ftn = subReq.fullTrackName;
  auto* localReg = &localRegistry();

  // Park before claiming this thread's forwarder, so a later SUBSCRIBE for the same
  // ftn is never bound to our timeout. The gate keeps the relayExec_ hop off the
  // common path: a ready local forwarder means a publisher already resolved.
  if (auto rendezvousTimeout = takeRendezvousTimeout(subReq, session);
      rendezvousTimeout && !localReg->getIfReady(ftn)) {
    if (auto rendezvousErr = co_await folly::coro::co_withExecutor(
            folly::getKeepAliveToken(relayExec_),
            rendezvousWithPublisherOrTimeout(subReq, *rendezvousTimeout)
        )) {
      co_return folly::makeUnexpected(std::move(*rendezvousErr));
    }
  }

  // Join before the relay hop: serializes same-iothread races.
  auto joined = localReg->join(ftn, [&] { return std::make_shared<MoQForwarder>(ftn); });

  consumer =
      wrapWithTrackStats(trackStats_, ftn, std::move(consumer), stats::TrackDirection::Egress);

  // Another setup is running on this thread, so wait for it to complete.  It's possible that
  // the first setup failed and another pending setup has been started, so loop until the entry
  // is ready, or fails.  This loop should terminate assuming the subscription gets established.
  while (auto* pending = std::get_if<LocalForwarderRegistry::Pending>(&joined)) {
    co_await folly::coro::co_awaitTry(std::move(pending->ready));
    auto state = localReg->lookup(ftn);
    if (auto* ready = std::get_if<LocalForwarderRegistry::Ready>(&state)) {
      joined = std::move(*ready);
    } else if (auto* reclaimed = std::get_if<LocalForwarderRegistry::Pending>(&state)) {
      joined = std::move(*reclaimed);
    } else {
      co_return folly::makeUnexpected(SubscribeError{
          subReq.requestID,
          SubscribeErrorCode::INTERNAL_ERROR,
          "local forwarder setup failed"
      });
    }
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
      attachNewLocalForwarderOnRelayExec(subReq, localReg, subscriberExec, crossExecFilter, forward)
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

std::optional<Publisher::TrackStatusResult>
LocalForwarderRelay::trackStatusOnSubscriberExec(const TrackStatus& req) {
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

Fetch LocalForwarderRelay::fetchOnSubscriberExec(
    Fetch fetch,
    const std::shared_ptr<MoQSession>& session
) {
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

folly::coro::Task<std::optional<TrackStatusOk>>
LocalForwarderRelay::readPublisherForwarderStatus(bool hasHandle, TrackStatus req) {
  auto* localReg = tlForwarders_.get();
  auto forwarder = localReg ? localReg->getIfReady(req.fullTrackName) : nullptr;
  if (!forwarder || forwarder->numForwardingSubscribers() == 0) {
    co_return std::nullopt;
  }
  co_return buildTrackStatusOk(*forwarder, hasHandle, req);
}

folly::coro::Task<std::optional<TrackStatusOk>> LocalForwarderRelay::readLocalTrackStatus(
    const SubscriptionRegistry::UpstreamView& upstreamView,
    const TrackStatus& req
) {
  // LF mode never touches registry->forwarder; read it via tlForwarders_ on the publisher exec.
  if (!upstreamView.publisherExec) {
    co_return std::nullopt;
  }
  co_return co_await folly::coro::co_withExecutor(
      folly::getKeepAliveToken(upstreamView.publisherExec),
      readPublisherForwarderStatus((bool)upstreamView.handle, req)
  );
}

void LocalForwarderRelay::evictOnOwner(
    const FullTrackName& ftn,
    const std::shared_ptr<MoQSession>& session,
    folly::Function<void(const std::shared_ptr<MoQForwarder>&)> evict
) {
  // The subscriber lives on the per-thread local forwarder, not the registry's
  // publisher forwarder; evict it on its owning exec.
  folly::via(session->getExecutor(), [this, ftn, evict = std::move(evict)]() mutable {
    auto* localReg = tlForwarders_.get();
    evict(localReg ? localReg->getForEviction(ftn) : nullptr);
  });
}

} // namespace openmoq::moqx
