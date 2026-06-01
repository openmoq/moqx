/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "moxygen/MoQConsumers.h"

#include <folly/futures/Future.h>

namespace moxygen {

// SubgroupConsumer that discards all calls.  Used as the terminal sink when
// a structural consumer (e.g. SubscribeWriteback) is required but no
// downstream delivery is needed.
class NullSubgroupConsumer : public SubgroupConsumer {
public:
  folly::Expected<folly::Unit, MoQPublishError>
  object(uint64_t, Payload, Extensions, bool) override {
    return folly::unit;
  }

  folly::Expected<folly::Unit, MoQPublishError>
  beginObject(uint64_t, uint64_t, Payload, Extensions) override {
    return folly::unit;
  }

  folly::Expected<ObjectPublishStatus, MoQPublishError> objectPayload(Payload, bool) override {
    return ObjectPublishStatus::IN_PROGRESS;
  }

  folly::Expected<folly::Unit, MoQPublishError> endOfGroup(uint64_t) override {
    return folly::unit;
  }

  folly::Expected<folly::Unit, MoQPublishError> endOfTrackAndGroup(uint64_t) override {
    return folly::unit;
  }

  folly::Expected<folly::Unit, MoQPublishError> endOfSubgroup() override { return folly::unit; }

  void reset(ResetStreamErrorCode) override {}
};

// TrackConsumer that discards all calls.  beginSubgroup() returns a
// NullSubgroupConsumer.
class NullTrackConsumer : public TrackConsumer {
public:
  folly::Expected<folly::Unit, MoQPublishError> setTrackAlias(TrackAlias) override {
    return folly::unit;
  }

  folly::Expected<std::shared_ptr<SubgroupConsumer>, MoQPublishError>
  beginSubgroup(uint64_t, uint64_t, Priority, bool) override {
    return std::make_shared<NullSubgroupConsumer>();
  }

  folly::Expected<folly::SemiFuture<folly::Unit>, MoQPublishError> awaitStreamCredit() override {
    return folly::makeSemiFuture();
  }

  folly::Expected<folly::Unit, MoQPublishError>
  objectStream(const ObjectHeader&, Payload, bool) override {
    return folly::unit;
  }

  folly::Expected<folly::Unit, MoQPublishError>
  datagram(const ObjectHeader&, Payload, bool) override {
    return folly::unit;
  }

  folly::Expected<folly::Unit, MoQPublishError> publishDone(PublishDone) override {
    return folly::unit;
  }
};

} // namespace moxygen
