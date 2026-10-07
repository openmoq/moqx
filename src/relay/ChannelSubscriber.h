/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/relay/MoQForwarder.h>

#include <folly/Executor.h>
#include <folly/logging/xlog.h>

#include <memory>
#include <optional>
#include <thread>

namespace openmoq::moqx {

// No-op when publisher has no channel under channelId.
inline void removeChannelSubscriber(
    moxygen::MoQForwarder& publisher,
    moxygen::SessionId channelId,
    std::optional<moxygen::PublishDone> pubDone = std::nullopt
) {
  if (publisher.getSubscriber(channelId)) {
    publisher.removeSubscriber(channelId, std::move(pubDone), "removeChannelSubscriber");
  }
}

// A local forwarder's channel subscription on a publisher's forwarder.  Must be
// constructed on the publisher's executor: the thread it captures there is the only
// one detach() will run on, and the weak_ptr is reachable nowhere else.  Safe to
// carry to other executors, since none of them can lock it.
class ChannelSubscriber {
public:
  ChannelSubscriber() = default;

  ChannelSubscriber(
      std::weak_ptr<moxygen::MoQForwarder> publisherFwd,
      folly::Executor* publisherExec
  )
      : publisherFwd_(std::move(publisherFwd)), publisherExec_(publisherExec),
        publisherThread_(std::this_thread::get_id()) {}

  folly::Executor* exec() const { return publisherExec_; }

  // No-op once the publisher's forwarder is gone; it took the subscription with it.
  void detach(moxygen::SessionId channelId) const {
    XCHECK_EQ(publisherThread_, std::this_thread::get_id())
        << "channel subscriber detached off the publisher's thread";
    if (auto publisher = publisherFwd_.lock()) {
      removeChannelSubscriber(*publisher, channelId);
    }
  }

private:
  std::weak_ptr<moxygen::MoQForwarder> publisherFwd_;
  folly::Executor* publisherExec_{nullptr};
  std::thread::id publisherThread_;
};

} // namespace openmoq::moqx
