/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "MoqxRelay.h"

namespace openmoq::moqx {

// One executor owns every forwarder, so the relay reads and writes them inline.
// That is relayExec_ when set, or else the single io thread.
class OwnedForwarderRelay final : public MoqxRelay {
public:
  OwnedForwarderRelay(
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
  void wireForwarderCallback(const std::shared_ptr<moxygen::MoQForwarder>& chainForwarder) override;
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
  std::optional<moxygen::FetchError> resolveJoiningFetchOnRelay(
      moxygen::Fetch& fetch,
      moxygen::JoiningFetch*& joining,
      const std::shared_ptr<moxygen::MoQSession>& session
  ) override;
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
  makePeerHandle(std::shared_ptr<moxygen::MoQForwarder::Subscriber> subscriber) override;

private:
  folly::coro::Task<SubscribeResult>
  subscribeImpl(moxygen::SubscribeRequest subReq, std::shared_ptr<moxygen::TrackConsumer> consumer);

  // Only set in single-threaded mode (relayExec_ == null); used as the
  // coroutine start executor for fire-and-forget tasks like doSubscribeUpdate.
  folly::Executor* sessionExec_{nullptr};

  void maybeSetSessionExec(moxygen::MoQSession& session) {
    if (!relayExec_ && !sessionExec_) {
      sessionExec_ = session.getExecutor();
    }
  }

  folly::Executor* relayExec() const { return relayExec_ ? relayExec_ : sessionExec_; }
};

} // namespace openmoq::moqx
