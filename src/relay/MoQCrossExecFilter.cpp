/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/MoQCrossExecFilter.h"

#include <folly/logging/xlog.h>

namespace openmoq::moqx {

namespace {

// Construct a MoQPublishError from a stored closeCode_ value.
moxygen::MoQPublishError closeError(uint32_t code) {
  return moxygen::MoQPublishError(static_cast<moxygen::MoQPublishError::Code>(code));
}

constexpr uint32_t kCancelled =
    static_cast<uint32_t>(moxygen::MoQPublishError::Code::CANCELLED);
constexpr uint32_t kWriteError =
    static_cast<uint32_t>(moxygen::MoQPublishError::Code::WRITE_ERROR);

} // namespace

// ---- MoQCrossExecSubgroupFilter ----

folly::Expected<folly::Unit, moxygen::MoQPublishError> MoQCrossExecSubgroupFilter::object(
    uint64_t objectID,
    moxygen::Payload payload,
    moxygen::Extensions extensions,
    bool finSubgroup
) {
  if (auto code = closeCode_.load(std::memory_order_relaxed)) {
    return folly::makeUnexpected(closeError(code));
  }
  targetExec_->add([self = shared_from_this(),
                    objectID,
                    payload = std::move(payload),
                    extensions = std::move(extensions),
                    finSubgroup]() mutable {
    if (!self->downstream_) {
      self->closeCode_.store(kCancelled, std::memory_order_relaxed);
      return;
    }
    auto result =
        self->downstream_->object(objectID, std::move(payload), std::move(extensions), finSubgroup);
    if (result.hasError()) {
      self->closeCode_.store(
          static_cast<uint32_t>(result.error().code), std::memory_order_relaxed);
    }
  });
  return folly::unit;
}

folly::Expected<folly::Unit, moxygen::MoQPublishError> MoQCrossExecSubgroupFilter::beginObject(
    uint64_t objectID,
    uint64_t length,
    moxygen::Payload initialPayload,
    moxygen::Extensions extensions
) {
  if (auto code = closeCode_.load(std::memory_order_relaxed)) {
    return folly::makeUnexpected(closeError(code));
  }
  payloadTracker_.beginObject(length, initialPayload);
  targetExec_->add([self = shared_from_this(),
                    objectID,
                    length,
                    payload = std::move(initialPayload),
                    extensions = std::move(extensions)]() mutable {
    if (!self->downstream_) {
      self->closeCode_.store(kCancelled, std::memory_order_relaxed);
      return;
    }
    auto result =
        self->downstream_->beginObject(objectID, length, std::move(payload), std::move(extensions));
    if (result.hasError()) {
      self->closeCode_.store(
          static_cast<uint32_t>(result.error().code), std::memory_order_relaxed);
    }
  });
  return folly::unit;
}

folly::Expected<moxygen::ObjectPublishStatus, moxygen::MoQPublishError>
MoQCrossExecSubgroupFilter::objectPayload(moxygen::Payload payload, bool finSubgroup) {
  if (auto code = closeCode_.load(std::memory_order_relaxed)) {
    return folly::makeUnexpected(closeError(code));
  }
  auto status = payloadTracker_.consume(payload);
  targetExec_->add(
      [self = shared_from_this(), payload = std::move(payload), finSubgroup]() mutable {
        if (!self->downstream_) {
          self->closeCode_.store(kCancelled, std::memory_order_relaxed);
          return;
        }
        auto result = self->downstream_->objectPayload(std::move(payload), finSubgroup);
        if (result.hasError()) {
          self->closeCode_.store(
              static_cast<uint32_t>(result.error().code), std::memory_order_relaxed);
        }
      });
  return status;
}

folly::Expected<folly::Unit, moxygen::MoQPublishError>
MoQCrossExecSubgroupFilter::endOfGroup(uint64_t endOfGroupObjectID) {
  targetExec_->add([self = shared_from_this(), endOfGroupObjectID]() {
    if (!self->downstream_) {
      return;
    }
    auto result = self->downstream_->endOfGroup(endOfGroupObjectID);
    if (result.hasError()) {
      self->closeCode_.store(
          static_cast<uint32_t>(result.error().code), std::memory_order_relaxed);
    }
  });
  return folly::unit;
}

folly::Expected<folly::Unit, moxygen::MoQPublishError>
MoQCrossExecSubgroupFilter::endOfTrackAndGroup(uint64_t endOfTrackObjectID) {
  targetExec_->add([self = shared_from_this(), endOfTrackObjectID]() {
    if (!self->downstream_) {
      return;
    }
    auto result = self->downstream_->endOfTrackAndGroup(endOfTrackObjectID);
    if (result.hasError()) {
      self->closeCode_.store(
          static_cast<uint32_t>(result.error().code), std::memory_order_relaxed);
    }
  });
  return folly::unit;
}

folly::Expected<folly::Unit, moxygen::MoQPublishError> MoQCrossExecSubgroupFilter::endOfSubgroup() {
  targetExec_->add([self = shared_from_this()]() {
    if (!self->downstream_) {
      return;
    }
    auto result = self->downstream_->endOfSubgroup();
    if (result.hasError()) {
      self->closeCode_.store(
          static_cast<uint32_t>(result.error().code), std::memory_order_relaxed);
    }
  });
  return folly::unit;
}

void MoQCrossExecSubgroupFilter::reset(moxygen::ResetStreamErrorCode error) {
  closeCode_.store(kCancelled, std::memory_order_relaxed);
  targetExec_->add([self = shared_from_this(), error]() {
    if (self->downstream_) {
      self->downstream_->reset(error);
    }
  });
}

void MoQCrossExecSubgroupFilter::checkpoint() {
  targetExec_->add([self = shared_from_this()]() {
    if (self->downstream_) {
      self->downstream_->checkpoint();
    }
  });
}

folly::Expected<folly::SemiFuture<uint64_t>, moxygen::MoQPublishError>
MoQCrossExecSubgroupFilter::awaitReadyToConsume() {
  if (auto code = closeCode_.load(std::memory_order_relaxed)) {
    return folly::makeUnexpected(closeError(code));
  }
  return folly::makeSemiFuture<uint64_t>(0);
}

// ---- MoQCrossExecFilter ----

folly::Expected<folly::Unit, moxygen::MoQPublishError>
MoQCrossExecFilter::setTrackAlias(moxygen::TrackAlias alias) {
  if (auto code = closeCode_.load(std::memory_order_relaxed)) {
    return folly::makeUnexpected(closeError(code));
  }
  targetExec_->add([self = shared_from_this(), alias]() {
    if (!self->downstream_) {
      self->closeCode_.store(kWriteError, std::memory_order_relaxed);
      return;
    }
    auto result = self->downstream_->setTrackAlias(alias);
    if (result.hasError()) {
      self->closeCode_.store(
          static_cast<uint32_t>(result.error().code), std::memory_order_relaxed);
    }
  });
  return folly::unit;
}

folly::Expected<std::shared_ptr<moxygen::SubgroupConsumer>, moxygen::MoQPublishError>
MoQCrossExecFilter::beginSubgroup(
    uint64_t groupID,
    uint64_t subgroupID,
    moxygen::Priority priority,
    bool containsLastInGroup
) {
  if (auto code = closeCode_.load(std::memory_order_relaxed)) {
    return folly::makeUnexpected(closeError(code));
  }
  auto subFilter = std::make_shared<MoQCrossExecSubgroupFilter>(targetExec_);
  targetExec_->add(
      [self = shared_from_this(), subFilter, groupID, subgroupID, priority, containsLastInGroup](
      ) mutable {
        if (!self->downstream_) {
          self->closeCode_.store(kWriteError, std::memory_order_relaxed);
          return;
        }
        auto result =
            self->downstream_->beginSubgroup(groupID, subgroupID, priority, containsLastInGroup);
        if (result.hasValue()) {
          subFilter->setDownstream(std::move(result.value()));
          subFilter->setKeepAlive(self->downstream_);
        } else {
          XLOG(ERR) << "MoQCrossExecFilter beginSubgroup failed: " << result.error().describe();
          self->closeCode_.store(
              static_cast<uint32_t>(result.error().code), std::memory_order_relaxed);
        }
      }
  );
  return subFilter;
}

folly::Expected<folly::SemiFuture<folly::Unit>, moxygen::MoQPublishError>
MoQCrossExecFilter::awaitStreamCredit() {
  if (auto code = closeCode_.load(std::memory_order_relaxed)) {
    return folly::makeUnexpected(closeError(code));
  }
  return folly::makeSemiFuture();
}

folly::Expected<folly::Unit, moxygen::MoQPublishError> MoQCrossExecFilter::objectStream(
    const moxygen::ObjectHeader& header,
    moxygen::Payload payload,
    bool lastInGroup
) {
  if (auto code = closeCode_.load(std::memory_order_relaxed)) {
    return folly::makeUnexpected(closeError(code));
  }
  targetExec_->add([self = shared_from_this(), header, payload = std::move(payload), lastInGroup](
                   ) mutable {
    if (!self->downstream_) {
      self->closeCode_.store(kWriteError, std::memory_order_relaxed);
      return;
    }
    auto result = self->downstream_->objectStream(header, std::move(payload), lastInGroup);
    if (result.hasError()) {
      self->closeCode_.store(
          static_cast<uint32_t>(result.error().code), std::memory_order_relaxed);
    }
  });
  return folly::unit;
}

folly::Expected<folly::Unit, moxygen::MoQPublishError> MoQCrossExecFilter::datagram(
    const moxygen::ObjectHeader& header,
    moxygen::Payload payload,
    bool lastInGroup
) {
  if (auto code = closeCode_.load(std::memory_order_relaxed)) {
    return folly::makeUnexpected(closeError(code));
  }
  targetExec_->add([self = shared_from_this(), header, payload = std::move(payload), lastInGroup](
                   ) mutable {
    if (!self->downstream_) {
      self->closeCode_.store(kWriteError, std::memory_order_relaxed);
      return;
    }
    auto result = self->downstream_->datagram(header, std::move(payload), lastInGroup);
    if (result.hasError()) {
      self->closeCode_.store(
          static_cast<uint32_t>(result.error().code), std::memory_order_relaxed);
    }
  });
  return folly::unit;
}

folly::Expected<folly::Unit, moxygen::MoQPublishError>
MoQCrossExecFilter::publishDone(moxygen::PublishDone pubDone) {
  targetExec_->add([self = shared_from_this(), pubDone = std::move(pubDone)]() mutable {
    if (self->downstream_) {
      self->downstream_->publishDone(std::move(pubDone));
    }
  });
  return folly::unit;
}

// ---- MoQFetchCrossExecFilter ----

folly::Expected<folly::Unit, moxygen::MoQPublishError> MoQFetchCrossExecFilter::object(
    uint64_t groupID,
    uint64_t subgroupID,
    uint64_t objectID,
    moxygen::Payload payload,
    moxygen::Extensions extensions,
    bool finFetch,
    bool forwardingPreferenceIsDatagram
) {
  if (auto code = closeCode_.load(std::memory_order_relaxed)) {
    return folly::makeUnexpected(closeError(code));
  }
  targetExec_->add([self = shared_from_this(),
                    groupID,
                    subgroupID,
                    objectID,
                    payload = std::move(payload),
                    extensions = std::move(extensions),
                    finFetch,
                    forwardingPreferenceIsDatagram]() mutable {
    if (!self->downstream_) {
      self->closeCode_.store(kWriteError, std::memory_order_relaxed);
      return;
    }
    auto result = self->downstream_->object(
        groupID,
        subgroupID,
        objectID,
        std::move(payload),
        std::move(extensions),
        finFetch,
        forwardingPreferenceIsDatagram);
    if (result.hasError()) {
      self->closeCode_.store(
          static_cast<uint32_t>(result.error().code), std::memory_order_relaxed);
    }
  });
  return folly::unit;
}

folly::Expected<folly::Unit, moxygen::MoQPublishError> MoQFetchCrossExecFilter::beginObject(
    uint64_t groupID,
    uint64_t subgroupID,
    uint64_t objectID,
    uint64_t length,
    moxygen::Payload initialPayload,
    moxygen::Extensions extensions
) {
  if (auto code = closeCode_.load(std::memory_order_relaxed)) {
    return folly::makeUnexpected(closeError(code));
  }
  payloadTracker_.beginObject(length, initialPayload);
  targetExec_->add([self = shared_from_this(),
                    groupID,
                    subgroupID,
                    objectID,
                    length,
                    payload = std::move(initialPayload),
                    extensions = std::move(extensions)]() mutable {
    if (!self->downstream_) {
      self->closeCode_.store(kWriteError, std::memory_order_relaxed);
      return;
    }
    auto result = self->downstream_->beginObject(
        groupID, subgroupID, objectID, length, std::move(payload), std::move(extensions));
    if (result.hasError()) {
      self->closeCode_.store(
          static_cast<uint32_t>(result.error().code), std::memory_order_relaxed);
    }
  });
  return folly::unit;
}

folly::Expected<moxygen::ObjectPublishStatus, moxygen::MoQPublishError>
MoQFetchCrossExecFilter::objectPayload(moxygen::Payload payload, bool finSubgroup) {
  if (auto code = closeCode_.load(std::memory_order_relaxed)) {
    return folly::makeUnexpected(closeError(code));
  }
  auto status = payloadTracker_.consume(payload);
  targetExec_->add(
      [self = shared_from_this(), payload = std::move(payload), finSubgroup]() mutable {
        if (!self->downstream_) {
          self->closeCode_.store(kWriteError, std::memory_order_relaxed);
          return;
        }
        auto result = self->downstream_->objectPayload(std::move(payload), finSubgroup);
        if (result.hasError()) {
          self->closeCode_.store(
              static_cast<uint32_t>(result.error().code), std::memory_order_relaxed);
        }
      });
  return status;
}

folly::Expected<folly::Unit, moxygen::MoQPublishError> MoQFetchCrossExecFilter::endOfGroup(
    uint64_t groupID,
    uint64_t subgroupID,
    uint64_t objectID,
    bool finFetch
) {
  targetExec_->add([self = shared_from_this(), groupID, subgroupID, objectID, finFetch]() {
    if (self->downstream_) {
      self->downstream_->endOfGroup(groupID, subgroupID, objectID, finFetch);
    }
  });
  return folly::unit;
}

folly::Expected<folly::Unit, moxygen::MoQPublishError>
MoQFetchCrossExecFilter::endOfTrackAndGroup(
    uint64_t groupID,
    uint64_t subgroupID,
    uint64_t objectID
) {
  targetExec_->add([self = shared_from_this(), groupID, subgroupID, objectID]() {
    if (self->downstream_) {
      self->downstream_->endOfTrackAndGroup(groupID, subgroupID, objectID);
    }
  });
  return folly::unit;
}

folly::Expected<folly::Unit, moxygen::MoQPublishError> MoQFetchCrossExecFilter::endOfFetch() {
  targetExec_->add([self = shared_from_this()]() {
    if (self->downstream_) {
      self->downstream_->endOfFetch();
    }
  });
  return folly::unit;
}

void MoQFetchCrossExecFilter::reset(moxygen::ResetStreamErrorCode error) {
  closeCode_.store(kCancelled, std::memory_order_relaxed);
  targetExec_->add([self = shared_from_this(), error]() {
    if (self->downstream_) {
      self->downstream_->reset(error);
    }
  });
}

folly::Expected<folly::SemiFuture<uint64_t>, moxygen::MoQPublishError>
MoQFetchCrossExecFilter::awaitReadyToConsume() {
  if (auto code = closeCode_.load(std::memory_order_relaxed)) {
    return folly::makeUnexpected(closeError(code));
  }
  return folly::makeSemiFuture<uint64_t>(0);
}

} // namespace openmoq::moqx
