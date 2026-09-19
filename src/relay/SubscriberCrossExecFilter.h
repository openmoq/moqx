/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/Executor.h>
#include <memory>
#include <moxygen/Subscriber.h>

namespace openmoq::moqx {

class CrossExecFilter;

// Forwards all Subscriber calls to a target executor.
// publishNamespace() switches to targetExec_ before invoking the inner.
// publish() returns a CrossExecFilter with null downstream immediately and a
// reply Task; driving the reply Task calls inner_->publish() on targetExec_,
// wires the real consumer via setDownstream(), then awaits the reply. FIFO
// ordering guarantees setDownstream() runs before any data writes enqueued
// afterward.
// goaway() is fire-and-forget.
//
// Requires targetExec_ to be a FIFO executor if call ordering matters.
//
// The owning session can close and drop this filter while a call is still out,
// so publishNamespace() passes a co_safe_point after its await, before it
// touches a member. That is safe only because MoQSession::cleanup requests
// cancellation before it releases the filter. publish() and goaway() need none:
// each copies what it uses out of the filter before it returns.
class SubscriberCrossExecFilter : public moxygen::Subscriber {
public:
  SubscriberCrossExecFilter(folly::Executor* targetExec, std::shared_ptr<moxygen::Subscriber> inner)
      : targetExec_(targetExec), inner_(std::move(inner)) {}

  folly::coro::Task<PublishNamespaceResult> publishNamespace(
      moxygen::PublishNamespace ann,
      std::shared_ptr<PublishNamespaceCallback> callback
  ) override;

  PublishResult publish(
      moxygen::PublishRequest pub,
      std::shared_ptr<moxygen::SubscriptionHandle> handle
  ) override;

  void goaway(moxygen::Goaway goaway) override;

private:
  static folly::coro::Task<folly::Expected<moxygen::PublishOk, moxygen::PublishError>>
  doPublishOnExec(
      std::shared_ptr<moxygen::Subscriber> inner,
      moxygen::PublishRequest pub,
      std::shared_ptr<moxygen::SubscriptionHandle> handle,
      std::shared_ptr<CrossExecFilter> filter
  );

  folly::Executor* targetExec_;
  std::shared_ptr<moxygen::Subscriber> inner_;
};

} // namespace openmoq::moqx
