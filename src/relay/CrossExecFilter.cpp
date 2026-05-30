/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/CrossExecFilter.h"

#include <folly/logging/xlog.h>

namespace openmoq::moqx {

namespace {

// Returns a coalesced deep copy of payload bytes when deepCopy is true,
// otherwise moves it. cloneCoalesced() copies bytes into a single contiguous
// buffer; unshare() ensures independence for single-element chains where
// coalesce() is a no-op and the buffer would otherwise still be shared.
moxygen::Payload maybeDeepCopy(moxygen::Payload& payload, bool deepCopy) {
  if (!deepCopy || !payload) {
    return std::move(payload);
  }
  auto copy = payload->cloneCoalesced();
  copy->unshare();
  return copy;
}

} // namespace

// ---- CrossExecSubgroupFilter ----

folly::Expected<folly::Unit, moxygen::MoQPublishError> CrossExecSubgroupFilter::object(
    uint64_t objectID,
    moxygen::Payload payload,
    moxygen::Extensions extensions,
    bool finSubgroup
) {
  if (auto err = loadDeferredError()) {
    enqueueDeactivate();
    return folly::makeUnexpected(*err);
  }
  auto capturedPayload = maybeDeepCopy(payload, deepCopyPayload_);
  targetExec_->add([this,
                    objectID,
                    payload = std::move(capturedPayload),
                    extensions = std::move(extensions),
                    finSubgroup]() mutable {
    if (downstream_) {
      auto result =
          downstream_->object(objectID, std::move(payload), std::move(extensions), finSubgroup);
      if (result.hasError()) {
        closeWithError(result.error(), std::move(downstream_));
      }
    }
    if (finSubgroup) {
      deactivate();
    }
  });
  return folly::unit;
}

folly::Expected<folly::Unit, moxygen::MoQPublishError> CrossExecSubgroupFilter::beginObject(
    uint64_t objectID,
    uint64_t length,
    moxygen::Payload initialPayload,
    moxygen::Extensions extensions
) {
  if (auto err = loadDeferredError()) {
    enqueueDeactivate();
    return folly::makeUnexpected(*err);
  }
  payloadTracker_.beginObject(length, initialPayload);
  auto capturedPayload = maybeDeepCopy(initialPayload, deepCopyPayload_);
  targetExec_->add([this,
                    objectID,
                    length,
                    payload = std::move(capturedPayload),
                    extensions = std::move(extensions)]() mutable {
    if (!downstream_) {
      return;
    }
    auto result =
        downstream_->beginObject(objectID, length, std::move(payload), std::move(extensions));
    if (result.hasError()) {
      closeWithError(result.error(), std::move(downstream_));
    }
  });
  return folly::unit;
}

folly::Expected<moxygen::ObjectPublishStatus, moxygen::MoQPublishError>
CrossExecSubgroupFilter::objectPayload(moxygen::Payload payload, bool finSubgroup) {
  if (auto err = loadDeferredError()) {
    enqueueDeactivate();
    return folly::makeUnexpected(*err);
  }
  auto status = payloadTracker_.consume(payload);
  auto capturedPayload = maybeDeepCopy(payload, deepCopyPayload_);
  targetExec_->add([this, payload = std::move(capturedPayload), finSubgroup]() mutable {
    if (downstream_) {
      auto result = downstream_->objectPayload(std::move(payload), finSubgroup);
      if (result.hasError()) {
        closeWithError(result.error(), std::move(downstream_));
      }
    }
    if (finSubgroup) {
      deactivate();
    }
  });
  return status;
}

folly::Expected<folly::Unit, moxygen::MoQPublishError>
CrossExecSubgroupFilter::endOfGroup(uint64_t endOfGroupObjectID) {
  if (auto err = loadDeferredError()) {
    enqueueDeactivate();
    return folly::makeUnexpected(*err);
  }
  targetExec_->add([this, endOfGroupObjectID]() {
    if (downstream_) {
      auto result = downstream_->endOfGroup(endOfGroupObjectID);
      if (result.hasError()) {
        // terminal — no storeDeferredError needed
        XLOG(ERR) << "endOfGroup: " << result.error().describe();
      }
    }
    deactivate();
  });
  return folly::unit;
}

folly::Expected<folly::Unit, moxygen::MoQPublishError>
CrossExecSubgroupFilter::endOfTrackAndGroup(uint64_t endOfTrackObjectID) {
  if (auto err = loadDeferredError()) {
    enqueueDeactivate();
    return folly::makeUnexpected(*err);
  }
  targetExec_->add([this, endOfTrackObjectID]() {
    if (downstream_) {
      auto result = downstream_->endOfTrackAndGroup(endOfTrackObjectID);
      if (result.hasError()) {
        // terminal — no storeDeferredError needed
        XLOG(ERR) << "endOfTrackAndGroup: " << result.error().describe();
      }
    }
    deactivate();
  });
  return folly::unit;
}

folly::Expected<folly::Unit, moxygen::MoQPublishError> CrossExecSubgroupFilter::endOfSubgroup() {
  if (auto err = loadDeferredError()) {
    enqueueDeactivate();
    return folly::makeUnexpected(*err);
  }
  targetExec_->add([this]() {
    if (downstream_) {
      auto result = downstream_->endOfSubgroup();
      if (result.hasError()) {
        // terminal — no storeDeferredError needed
        XLOG(ERR) << "endOfSubgroup: " << result.error().describe();
      }
    }
    deactivate();
  });
  return folly::unit;
}

void CrossExecSubgroupFilter::reset(moxygen::ResetStreamErrorCode error) {
  // storeDeferredError on calling thread; lambda needs no storeDeferredError even if downstream_ is
  // null
  storeDeferredError(moxygen::MoQPublishError::CANCELLED);
  targetExec_->add([self = shared_from_this(), error]() {
    if (self->downstream_) {
      self->downstream_->reset(error);
    }
    self->deactivate();
  });
}

void CrossExecSubgroupFilter::checkpoint() {
  if (loadDeferredError()) {
    enqueueDeactivate();
    return;
  }
  targetExec_->add([this]() {
    if (downstream_) {
      downstream_->checkpoint();
    }
  });
}

folly::Expected<folly::SemiFuture<uint64_t>, moxygen::MoQPublishError>
CrossExecSubgroupFilter::awaitReadyToConsume() {
  if (auto err = loadDeferredError()) {
    enqueueDeactivate();
    return folly::makeUnexpected(*err);
  }
  // TODO: backpressure not implemented; always reports ready
  return folly::makeSemiFuture<uint64_t>(0);
}

// ---- CrossExecFilter ----

folly::Expected<folly::Unit, moxygen::MoQPublishError>
CrossExecFilter::setTrackAlias(moxygen::TrackAlias alias) {
  if (auto err = loadDeferredError()) {
    return folly::makeUnexpected(*err);
  }
  targetExec_->add([this, alias]() {
    if (!downstream_) {
      storeDeferredError(moxygen::MoQPublishError::WRITE_ERROR);
      return;
    }
    auto result = downstream_->setTrackAlias(alias);
    if (result.hasError()) {
      storeDeferredError(result.error());
    }
  });
  return folly::unit;
}

folly::Expected<std::shared_ptr<moxygen::SubgroupConsumer>, moxygen::MoQPublishError>
CrossExecFilter::beginSubgroup(
    uint64_t groupID,
    uint64_t subgroupID,
    moxygen::Priority priority,
    bool containsLastInGroup
) {
  if (auto err = loadDeferredError()) {
    return folly::makeUnexpected(*err);
  }
  auto subFilter = CrossExecSubgroupFilter::create(targetExec_, deepCopyPayload_);
  targetExec_->add([this, subFilter, groupID, subgroupID, priority, containsLastInGroup]() mutable {
    if (!downstream_) {
      subFilter->closeWithError(moxygen::MoQPublishError::WRITE_ERROR);
      return;
    }
    auto result = downstream_->beginSubgroup(groupID, subgroupID, priority, containsLastInGroup);
    if (result.hasValue()) {
      subFilter->setDownstream(std::move(result.value()));
      subFilter->setKeepAlive(downstream_);
    } else {
      XLOG(ERR) << "CrossExecFilter beginSubgroup failed: " << result.error().describe();
      subFilter->closeWithError(result.error());
    }
  });
  return subFilter;
}

folly::Expected<folly::SemiFuture<folly::Unit>, moxygen::MoQPublishError>
CrossExecFilter::awaitStreamCredit() {
  if (auto err = loadDeferredError()) {
    return folly::makeUnexpected(*err);
  }
  // TODO: stream credit not forwarded across executor; always reports ready
  return folly::makeSemiFuture();
}

folly::Expected<folly::Unit, moxygen::MoQPublishError> CrossExecFilter::objectStream(
    const moxygen::ObjectHeader& header,
    moxygen::Payload payload,
    bool lastInGroup
) {
  if (auto err = loadDeferredError()) {
    return folly::makeUnexpected(*err);
  }
  auto capturedPayload = maybeDeepCopy(payload, deepCopyPayload_);
  targetExec_->add([this, header, payload = std::move(capturedPayload), lastInGroup]() mutable {
    if (!downstream_) {
      objectStreamErrors_.fetch_add(1, std::memory_order_relaxed);
      XLOG(ERR) << "objectStream: no downstream";
      return;
    }
    auto result = downstream_->objectStream(header, std::move(payload), lastInGroup);
    if (result.hasError()) {
      objectStreamErrors_.fetch_add(1, std::memory_order_relaxed);
      XLOG(ERR) << "objectStream: " << result.error().describe();
    }
  });
  return folly::unit;
}

folly::Expected<folly::Unit, moxygen::MoQPublishError> CrossExecFilter::datagram(
    const moxygen::ObjectHeader& header,
    moxygen::Payload payload,
    bool lastInGroup
) {
  if (auto err = loadDeferredError()) {
    return folly::makeUnexpected(*err);
  }
  auto capturedPayload = maybeDeepCopy(payload, deepCopyPayload_);
  targetExec_->add([this, header, payload = std::move(capturedPayload), lastInGroup]() mutable {
    if (!downstream_) {
      datagramErrors_.fetch_add(1, std::memory_order_relaxed);
      XLOG(ERR) << "datagram: no downstream";
      return;
    }
    auto result = downstream_->datagram(header, std::move(payload), lastInGroup);
    if (result.hasError()) {
      datagramErrors_.fetch_add(1, std::memory_order_relaxed);
      XLOG(ERR) << "datagram: " << result.error().describe();
    }
  });
  return folly::unit;
}

folly::Expected<folly::Unit, moxygen::MoQPublishError>
CrossExecFilter::publishDone(moxygen::PublishDone pubDone) {
  // shared_from_this: always terminal; caller may drop its ref before the
  // lambda executes on targetExec_.
  targetExec_->add([self = shared_from_this(), pubDone = std::move(pubDone)]() mutable {
    if (self->downstream_) {
      self->downstream_->publishDone(std::move(pubDone));
    }
  });
  return folly::unit;
}

// ---- FetchCrossExecFilter ----

folly::Expected<folly::Unit, moxygen::MoQPublishError> FetchCrossExecFilter::object(
    uint64_t groupID,
    uint64_t subgroupID,
    uint64_t objectID,
    moxygen::Payload payload,
    moxygen::Extensions extensions,
    bool finFetch,
    bool forwardingPreferenceIsDatagram
) {
  if (auto err = loadDeferredError()) {
    enqueueDeactivate();
    return folly::makeUnexpected(*err);
  }
  auto capturedPayload = maybeDeepCopy(payload, deepCopyPayload_);
  targetExec_->add([this,
                    groupID,
                    subgroupID,
                    objectID,
                    payload = std::move(capturedPayload),
                    extensions = std::move(extensions),
                    finFetch,
                    forwardingPreferenceIsDatagram]() mutable {
    if (downstream_) {
      auto result = downstream_->object(
          groupID,
          subgroupID,
          objectID,
          std::move(payload),
          std::move(extensions),
          finFetch,
          forwardingPreferenceIsDatagram
      );
      if (result.hasError()) {
        closeWithError(result.error(), std::move(downstream_));
      }
    }
    if (finFetch) {
      deactivate();
    }
  });
  return folly::unit;
}

folly::Expected<folly::Unit, moxygen::MoQPublishError> FetchCrossExecFilter::beginObject(
    uint64_t groupID,
    uint64_t subgroupID,
    uint64_t objectID,
    uint64_t length,
    moxygen::Payload initialPayload,
    moxygen::Extensions extensions
) {
  if (auto err = loadDeferredError()) {
    enqueueDeactivate();
    return folly::makeUnexpected(*err);
  }
  payloadTracker_.beginObject(length, initialPayload);
  auto capturedPayload = maybeDeepCopy(initialPayload, deepCopyPayload_);
  targetExec_->add([this,
                    groupID,
                    subgroupID,
                    objectID,
                    length,
                    payload = std::move(capturedPayload),
                    extensions = std::move(extensions)]() mutable {
    if (!downstream_) {
      return;
    }
    auto result = downstream_->beginObject(
        groupID,
        subgroupID,
        objectID,
        length,
        std::move(payload),
        std::move(extensions)
    );
    if (result.hasError()) {
      closeWithError(result.error(), std::move(downstream_));
    }
  });
  return folly::unit;
}

folly::Expected<moxygen::ObjectPublishStatus, moxygen::MoQPublishError>
FetchCrossExecFilter::objectPayload(moxygen::Payload payload, bool finFetch) {
  if (auto err = loadDeferredError()) {
    enqueueDeactivate();
    return folly::makeUnexpected(*err);
  }
  auto status = payloadTracker_.consume(payload);
  auto capturedPayload = maybeDeepCopy(payload, deepCopyPayload_);
  targetExec_->add([this, payload = std::move(capturedPayload), finFetch]() mutable {
    if (downstream_) {
      auto result = downstream_->objectPayload(std::move(payload), finFetch);
      if (result.hasError()) {
        closeWithError(result.error(), std::move(downstream_));
      }
    }
    if (finFetch) {
      deactivate();
    }
  });
  return status;
}

folly::Expected<folly::Unit, moxygen::MoQPublishError> FetchCrossExecFilter::endOfGroup(
    uint64_t groupID,
    uint64_t subgroupID,
    uint64_t objectID,
    bool finFetch
) {
  if (auto err = loadDeferredError()) {
    enqueueDeactivate();
    return folly::makeUnexpected(*err);
  }
  targetExec_->add([this, groupID, subgroupID, objectID, finFetch]() {
    if (downstream_) {
      auto result = downstream_->endOfGroup(groupID, subgroupID, objectID, finFetch);
      if (result.hasError()) {
        closeWithError(result.error(), std::move(downstream_));
      }
    }
    if (finFetch) {
      deactivate();
    }
  });
  return folly::unit;
}

folly::Expected<folly::Unit, moxygen::MoQPublishError>
FetchCrossExecFilter::endOfTrackAndGroup(uint64_t groupID, uint64_t subgroupID, uint64_t objectID) {
  if (auto err = loadDeferredError()) {
    enqueueDeactivate();
    return folly::makeUnexpected(*err);
  }
  targetExec_->add([this, groupID, subgroupID, objectID]() {
    if (downstream_) {
      auto result = downstream_->endOfTrackAndGroup(groupID, subgroupID, objectID);
      if (result.hasError()) {
        XLOG(ERR) << "endOfTrackAndGroup: " << result.error().describe();
      }
    }
    deactivate();
  });
  return folly::unit;
}

folly::Expected<folly::Unit, moxygen::MoQPublishError> FetchCrossExecFilter::endOfFetch() {
  if (auto err = loadDeferredError()) {
    enqueueDeactivate();
    return folly::makeUnexpected(*err);
  }
  targetExec_->add([this]() {
    if (downstream_) {
      auto result = downstream_->endOfFetch();
      if (result.hasError()) {
        XLOG(ERR) << "endOfFetch: " << result.error().describe();
      }
    }
    deactivate();
  });
  return folly::unit;
}

void FetchCrossExecFilter::checkpoint() {
  if (loadDeferredError()) {
    enqueueDeactivate();
    return;
  }
  targetExec_->add([this]() {
    if (downstream_) {
      downstream_->checkpoint();
    }
  });
}

folly::Expected<folly::Unit, moxygen::MoQPublishError>
FetchCrossExecFilter::endOfUnknownRange(uint64_t groupID, uint64_t objectID, bool finFetch) {
  if (auto err = loadDeferredError()) {
    enqueueDeactivate();
    return folly::makeUnexpected(*err);
  }
  targetExec_->add([this, groupID, objectID, finFetch]() {
    if (downstream_) {
      auto result = downstream_->endOfUnknownRange(groupID, objectID, finFetch);
      if (result.hasError()) {
        closeWithError(result.error(), std::move(downstream_));
      }
    }
    if (finFetch) {
      deactivate();
    }
  });
  return folly::unit;
}

void FetchCrossExecFilter::reset(moxygen::ResetStreamErrorCode error) {
  // storeDeferredError on calling thread so subsequent calls fail fast.
  // shared_from_this: reset() bypasses loadDeferredError(), so the source may
  // call it after enqueueDeactivate() has already moved selfGuard_ out; we need an
  // independent ref to guarantee the object outlives the lambda.
  storeDeferredError(moxygen::MoQPublishError::CANCELLED);
  targetExec_->add([self = shared_from_this(), error]() {
    if (self->downstream_) {
      self->downstream_->reset(error);
    }
    self->deactivate();
  });
}

folly::Expected<folly::SemiFuture<uint64_t>, moxygen::MoQPublishError>
FetchCrossExecFilter::awaitReadyToConsume() {
  if (auto err = loadDeferredError()) {
    enqueueDeactivate();
    return folly::makeUnexpected(*err);
  }
  // TODO: backpressure not implemented; always reports ready
  return folly::makeSemiFuture<uint64_t>(0);
}

} // namespace openmoq::moqx
