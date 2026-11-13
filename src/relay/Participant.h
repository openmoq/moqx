/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/MoQFramer.h>
#include <moxygen/MoQTypes.h>
#include <moxygen/Publisher.h>
#include <moxygen/Subscriber.h>
#include <moxygen/openmoq/extensions/MoQClusterExtension.h>

#include <folly/Executor.h>
#include <folly/coro/Invoke.h>
#include <folly/coro/Task.h>
#include <folly/io/async/Request.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace openmoq::moqx {

class RouteCodec;

// Routes that touch a participant: routes it announced, and routes whose path runs through
// its hop.
struct RouteExclusion {
  std::optional<moxygen::SessionId> session;   // routes announced on this session
  uint64_t hop{moxygen::kMoQClusterAnonHopId}; // routes whose path contains it; 0 disables it
};

// How a participant's routes are read, written and filtered.
struct ParticipantRouting {
  // From the session's SETUP.
  bool relayHops{false};                             // negotiated MoQ Cluster
  uint64_t peerHopID{moxygen::kMoQClusterAnonHopId}; // its MoQ Cluster Hop ID
  uint64_t linkCost{1};                              // added to each route it announces

  // Chosen by PeeringBackend::routingFor().
  std::shared_ptr<const RouteCodec> codec;
  // Its routes are learned through relay peering, so they coexist with other routes.
  bool fromPeering{false};
  // The relay re-advertises an announcement to it when the best route changes.
  bool wantsUpdates{false};
  // Routes that its requests skip and that the relay does not advertise to it.
  std::optional<RouteExclusion> exclusion;
};

// Facts about whoever is on the other end of a request, taken at its first request. It is a value,
// so relay code can read it on any thread and cannot call the participant through it.
struct ParticipantInfo {
  moxygen::SessionId sessionId;
  uint64_t version{0};
  folly::Executor* executor{nullptr};
  std::string address;
  std::string authority;
  ParticipantRouting routing;
};

// What the relay keeps about a participant. publisher and subscriber are callable from the relay's
// thread, and dropping them does not end the participant.
class Participant {
public:
  // Calls into the participant without an executor hop. Only runOnExecutor creates one. It is
  // move-only, so a lambda cannot capture it by copy.
  class OnExecutor {
  public:
    OnExecutor(OnExecutor&&) = default;
    OnExecutor& operator=(OnExecutor&&) = default;
    OnExecutor(const OnExecutor&) = delete;
    OnExecutor& operator=(const OnExecutor&) = delete;

    moxygen::Publisher& publisher() const { return *publisher_; }
    moxygen::Subscriber& subscriber() const { return *subscriber_; }

  private:
    friend class Participant;
    OnExecutor(std::shared_ptr<moxygen::Publisher> p, std::shared_ptr<moxygen::Subscriber> s)
        : publisher_(std::move(p)), subscriber_(std::move(s)) {}

    std::shared_ptr<moxygen::Publisher> publisher_;
    std::shared_ptr<moxygen::Subscriber> subscriber_;
  };

  Participant() = default;
  // The on-executor pair defaults to publisher and subscriber.
  Participant(
      std::shared_ptr<const ParticipantInfo> info,
      std::shared_ptr<moxygen::Publisher> publisher,
      std::shared_ptr<moxygen::Subscriber> subscriber,
      std::shared_ptr<moxygen::Publisher> onExecutorPublisher = nullptr,
      std::shared_ptr<moxygen::Subscriber> onExecutorSubscriber = nullptr
  )
      : info(std::move(info)), publisher(std::move(publisher)), subscriber(std::move(subscriber)),
        onExecutorPublisher_(
            onExecutorPublisher ? std::move(onExecutorPublisher) : this->publisher
        ),
        onExecutorSubscriber_(
            onExecutorSubscriber ? std::move(onExecutorSubscriber) : this->subscriber
        ) {}

  // Runs fn(OnExecutor) as a task on info->executor.
  template <typename Fn> auto runOnExecutor(Fn fn) const {
    // co_invoke keeps fn, and so a coroutine lambda's captures, alive until the task finishes.
    return folly::coro::co_withExecutor(
        folly::getKeepAliveToken(info->executor),
        folly::coro::co_invoke(
            std::move(fn),
            OnExecutor(onExecutorPublisher_, onExecutorSubscriber_)
        )
    );
  }

  std::shared_ptr<const ParticipantInfo> info;
  std::shared_ptr<moxygen::Publisher> publisher;
  std::shared_ptr<moxygen::Subscriber> subscriber;

private:
  std::shared_ptr<moxygen::Publisher> onExecutorPublisher_;
  std::shared_ptr<moxygen::Subscriber> onExecutorSubscriber_;
};

// The participant follows the request across executor hops. XCHECKs outside a participant's
// request.
const Participant& requestParticipant();

// The caller must already be in a per-request context.
void attachParticipant(Participant participant);

// Attaches the participant to each REQUEST_UPDATE, which can carry a fill that the relay serves as
// this participant.
class ParticipantSubscriptionHandle : public moxygen::SubscriptionHandle {
public:
  ParticipantSubscriptionHandle(
      std::shared_ptr<moxygen::SubscriptionHandle> inner,
      Participant participant
  )
      : inner_(std::move(inner)), participant_(std::move(participant)) {
    if (inner_->hasSubscribeOk()) {
      setSubscribeOk(inner_->subscribeOk());
    }
  }

  const moxygen::SubscribeOk& subscribeOk() const override { return inner_->subscribeOk(); }

  void unsubscribe() override { inner_->unsubscribe(); }

  void cancelFill(moxygen::RequestID requestID) override { inner_->cancelFill(requestID); }

  // A coroutine, so the participant attaches to the context that runs the update.
  folly::coro::Task<RequestUpdateResult> requestUpdate(moxygen::RequestUpdate update) override {
    attachParticipant(participant_);
    co_return co_await inner_->requestUpdate(std::move(update));
  }

private:
  std::shared_ptr<moxygen::SubscriptionHandle> inner_;
  Participant participant_;
};

// Creates a request context for a caller that a session did not start, such as a peering backend.
class ParticipantScope {
public:
  explicit ParticipantScope(Participant participant);

private:
  folly::RequestContextScopeGuard guard_;
};

// Draws from the sequence that MoQSession ids come from, so ids stay unique across both.
moxygen::SessionId makeSessionId();

} // namespace openmoq::moqx
