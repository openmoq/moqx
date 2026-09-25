/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "MoqxRelay.h"
#include "relay/ChannelSubscriber.h"
#include "relay/InitialTrackState.h"
#include "relay/LocalForwarderRegistry.h"

#include <folly/ThreadLocal.h>

namespace openmoq::moqx {

class CrossExecFilter;

// Relay state runs on relayExec_, and per-thread local forwarders shortcut the data plane.
class LocalForwarderRelay : public MoqxRelay {
public:
  LocalForwarderRelay(
      config::CacheConfig cache,
      std::string relayID,
      uint64_t relayHopID,
      std::shared_ptr<folly::Executor> relayExec,
      uint64_t maxDeselected,
      std::chrono::milliseconds idleTimeout,
      std::chrono::milliseconds activityThreshold
  );

  std::shared_ptr<moxygen::Publisher> createPublisherFilter() override;
  std::shared_ptr<moxygen::Subscriber> createSubscriberFilter() override;

  // Unreachable: LocalSubscribeFilter and LocalPublishFilter intercept both.
  folly::coro::Task<SubscribeResult> subscribe(
      moxygen::SubscribeRequest subReq,
      std::shared_ptr<moxygen::TrackConsumer> consumer
  ) override;
  PublishResult publish(
      moxygen::PublishRequest pubReq,
      std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle = nullptr
  ) override;

protected:
  void releasePublisherEntry(const moxygen::FullTrackName& ftn) override;
  // Local forwarder already had its CrossExecForwarderCallback installed by
  // publishFromPublisherExec (dispatches onEmpty to relayExec_); don't overwrite.
  void wireForwarderCallback(const std::shared_ptr<moxygen::MoQForwarder>&) override {}
  // In LF mode the ref supplies only the track name and owning exec; the forwarder
  // itself is resolved by name there, so it doesn't attach to a displaced one.
  bool addSubscriberAndPublish(
      std::shared_ptr<moxygen::MoQSession> subscriberSession,
      const ForwarderRef& publisherRef,
      bool forward,
      bool pinned
  ) override;
  ForwarderRef makeForwarderRef(
      const std::shared_ptr<moxygen::MoQForwarder>& forwarder,
      folly::Executor* publisherExec
  ) const override;
  SubscriptionRegistry::FilterChainResult buildFilterChain(
      const moxygen::FullTrackName& ftn,
      std::shared_ptr<moxygen::MoQForwarder> forwarder
  ) override;
  // LF mode resolves/defers joining in fetchOnSubscriberExec on the subscriber exec.
  std::optional<moxygen::FetchError>
  resolveJoiningFetchOnRelay(moxygen::Fetch&, moxygen::JoiningFetch*&, const std::shared_ptr<moxygen::MoQSession>&)
      override {
    return std::nullopt;
  }
  folly::coro::Task<std::optional<moxygen::TrackStatusOk>> readLocalTrackStatus(
      const SubscriptionRegistry::UpstreamView& upstreamView,
      const moxygen::TrackStatus& req
  ) override;
  void evictOnOwner(
      const moxygen::FullTrackName& ftn,
      const std::shared_ptr<moxygen::MoQSession>& session,
      folly::Function<void(const std::shared_ptr<moxygen::MoQForwarder>&)> evict
  ) override;
  std::shared_ptr<moxygen::Publisher::SubscriptionHandle>
  makePeerHandle(std::shared_ptr<moxygen::MoQForwarder::Subscriber> subscriber) override {
    return subscriber;
  }

private:
  class LocalSubscribeFilter;
  class LocalPublishFilter;

  std::shared_ptr<LocalForwarderRelay> sharedSelf() {
    return std::static_pointer_cast<LocalForwarderRelay>(shared_from_this());
  }

  // This thread's registry, created on first use.
  LocalForwarderRegistry& localRegistry();

  struct LocalForwarderBootstrap {
    std::shared_ptr<moxygen::MoQForwarder> localFwd;
    bool isNew{false};
    LocalForwarderRegistry* localReg{nullptr};
  };
  LocalForwarderBootstrap
  acquireLocalForwarder(const moxygen::FullTrackName& ftn, const InitialTrackState& initial);

  struct ResolvedPublisher {
    ForwarderRef ref;
    ChannelSubscriber channelSub;
    InitialTrackState initial;
  };
  // Launch on track.exec: reads the publisher forwarder's state where it is stable to
  // initialize a new subscriber forwarder during publish fan-out.
  folly::coro::Task<std::optional<ResolvedPublisher>> resolvePublisherOnItsExec(TrackRef track);

  folly::coro::Task<void> addSubscriberAndPublishViaLocalForwarder(
      std::shared_ptr<moxygen::MoQSession> subscriberSession,
      TrackRef track,
      bool forward,
      bool pinned
  );

  moxygen::Subscriber::PublishResult publishFromPublisherExec(
      moxygen::PublishRequest pub,
      std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle,
      std::shared_ptr<moxygen::MoQSession> session
  );

  // Called from publish path or first subscriber path on publisher's exec.
  // The displaced forwarder (if any) is released on the publisher's exec after the
  // new forwarder is registered with the relay's registry.
  enum class InstallKind { FromPublish, FromSubscribe };
  LocalForwarderRegistry::ParkResult installPublisherForwarder(
      const moxygen::FullTrackName& ftn,
      const std::shared_ptr<moxygen::MoQForwarder>& fwd,
      InstallKind kind
  );

  folly::coro::Task<folly::Expected<moxygen::PublishOk, moxygen::PublishError>>
  registerPublishOnRelayExec(
      moxygen::PublishRequest pub,
      std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle,
      std::shared_ptr<moxygen::MoQSession> session,
      ForwarderRef publisherRef,
      std::shared_ptr<CrossExecFilter> relayChainFilter
  );

  // Result of joinOrPrepareUpstreamSubscription (runs on relayExec_).
  struct StatefulSubscribeResult {
    folly::Executor* publisherExec{nullptr}; // owning executor of the publisher forwarder
    std::optional<SubscribeResult> error;    // set on failure

    // Set only for the FirstSubscriber path. Consumed by
    // attachNewLocalForwarderOnRelayExec's publisherExec sortie (passive relay chain +
    // upstream subscribe). Pending destructor fires on abandoned move.
    struct FirstSubscriberSetup {
      std::shared_ptr<moxygen::MoQForwarder> publisherForwarder;
      std::shared_ptr<moxygen::MoQSession> upstreamSession;
      moxygen::SubscribeRequest upstreamSubReq;
      std::shared_ptr<moxygen::TrackConsumer> upstreamConsumer;
      SubscriptionRegistry::UpstreamSubscribePending pending;
      moxygen::RequestID clientRequestID;
    };
    std::optional<FirstSubscriberSetup> firstSetup;
  };

  folly::coro::Task<StatefulSubscribeResult>
  joinOrPrepareUpstreamSubscription(moxygen::SubscribeRequest subReq);

  // Output of attachNewLocalForwarderOnRelayExec, read on the subscriberExec tail. The relay
  // chain filter is not exposed (setDownstream/teardown happen inside attach); the tail
  // needs only ownsRelayChain to gate the sawOnEmpty teardown.
  struct PublisherAttachment {
    ForwarderRef publisherRef;
    bool ownsRelayChain{false}; // firstSetup path installed the passive relay chain
    std::shared_ptr<moxygen::MoQForwarder::Callback> finalCallback;
    // Captured off the publisher forwarder on its own exec, the only race-free place.
    InitialTrackState initial;
    std::optional<SubscribeResult> error; // set => bail
  };

  folly::coro::Task<PublisherAttachment> attachNewLocalForwarderOnRelayExec(
      const moxygen::SubscribeRequest& subReq,
      LocalForwarderRegistry* subscriberReg,
      folly::Executor* subscriberExec,
      std::shared_ptr<CrossExecFilter> crossExecFilter,
      bool forward
  );

  folly::coro::Task<SubscribeResult> subscribeFromSubscriberExec(
      moxygen::SubscribeRequest subReq,
      std::shared_ptr<moxygen::TrackConsumer> consumer,
      std::shared_ptr<moxygen::MoQSession> session,
      folly::Executor* subscriberExec
  );

  // Answers from this thread's local forwarder (race-free); nullopt defers to trackStatusImpl.
  std::optional<moxygen::Publisher::TrackStatusResult>
  trackStatusOnSubscriberExec(const moxygen::TrackStatus& req);

  // Resolves a joining fetch against this thread's local forwarder (race-free). Rewrites to a
  // standalone Fetch when largest is known, else clears joiningRequestID to defer to upstream.
  moxygen::Fetch
  fetchOnSubscriberExec(moxygen::Fetch fetch, const std::shared_ptr<moxygen::MoQSession>& session);

  // Must be scheduled on the publisher exec; looks the forwarder up in tlForwarders_ by FTN.
  // nullopt means no active sub, defer upstream.
  folly::coro::Task<std::optional<moxygen::TrackStatusOk>>
  readPublisherForwarderStatus(bool hasHandle, moxygen::TrackStatus req);

  folly::ThreadLocalPtr<LocalForwarderRegistry> tlForwarders_;
};

} // namespace openmoq::moqx
