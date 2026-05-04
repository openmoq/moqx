/*
 * Copyright (c) Synamedia
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <moxygen/MoQConsumers.h>
#include <folly/Function.h>

namespace openmoq::moqx {

// Thin TrackConsumer that fires a callback whenever a new subgroup begins.
// Used to observe group availability on the target forwarder without subscribing
// the actual session to that track. Removed from the forwarder once G_switch
// is found, before live delivery re-adds the session with the real consumer.
class GroupStartObserver : public moxygen::TrackConsumer {
 public:
  using Callback = folly::Function<void(uint64_t groupID)>;
  explicit GroupStartObserver(Callback cb) : callback_(std::move(cb)) {}

  folly::Expected<folly::Unit, moxygen::MoQPublishError>
  setTrackAlias(moxygen::TrackAlias) override {
    return folly::unit;
  }

  folly::Expected<
      std::shared_ptr<moxygen::SubgroupConsumer>,
      moxygen::MoQPublishError>
  beginSubgroup(
      uint64_t groupID,
      uint64_t /*subgroupID*/,
      moxygen::Priority,
      bool /*containsLastInGroup*/ = false) override {
    if (callback_) {
      callback_(groupID);
    }
    return std::make_shared<NoopSubgroup>();
  }

  folly::Expected<folly::SemiFuture<folly::Unit>, moxygen::MoQPublishError>
  awaitStreamCredit() override {
    return folly::SemiFuture<folly::Unit>(folly::unit);
  }

  folly::Expected<folly::Unit, moxygen::MoQPublishError>
  objectStream(const moxygen::ObjectHeader&, moxygen::Payload, bool) override {
    return folly::unit;
  }

  folly::Expected<folly::Unit, moxygen::MoQPublishError>
  datagram(const moxygen::ObjectHeader&, moxygen::Payload, bool) override {
    return folly::unit;
  }

  folly::Expected<folly::Unit, moxygen::MoQPublishError>
  publishDone(moxygen::PublishDone) override {
    return folly::unit;
  }

 private:
  Callback callback_;

  struct NoopSubgroup : public moxygen::SubgroupConsumer {
    folly::Expected<folly::Unit, moxygen::MoQPublishError>
    object(uint64_t, moxygen::Payload, moxygen::Extensions, bool) override {
      return folly::unit;
    }
    folly::Expected<folly::Unit, moxygen::MoQPublishError>
    beginObject(uint64_t, uint64_t, moxygen::Payload, moxygen::Extensions)
        override {
      return folly::unit;
    }
    folly::Expected<moxygen::ObjectPublishStatus, moxygen::MoQPublishError>
    objectPayload(moxygen::Payload, bool) override {
      return moxygen::ObjectPublishStatus::DONE;
    }
    folly::Expected<folly::Unit, moxygen::MoQPublishError>
    endOfGroup(uint64_t) override {
      return folly::unit;
    }
    folly::Expected<folly::Unit, moxygen::MoQPublishError>
    endOfTrackAndGroup(uint64_t) override {
      return folly::unit;
    }
    folly::Expected<folly::Unit, moxygen::MoQPublishError>
    endOfSubgroup() override {
      return folly::unit;
    }
    void reset(moxygen::ResetStreamErrorCode) override {}
  };
};

} // namespace openmoq::moqx
