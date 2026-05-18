/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "relay/SubgroupConsumerUtil.h"

#include <atomic>
#include <folly/Executor.h>
#include <memory>
#include <moxygen/MoQFilters.h>

namespace openmoq::moqx {

// Forwards all SubgroupConsumer calls to a target executor (fire-and-forget).
// downstream_ starts null and is populated by MoQCrossExecFilter::beginSubgroup()
// on the target executor. FIFO ordering guarantees it is set before any
// object/endOf* calls enqueued afterward execute.
//
// closeCode_ is written on the target executor when downstream returns an error
// or is null (meaning beginSubgroup failed), and read on the calling thread.
// It holds a MoQPublishError::Code cast to uint32_t; 0 means open.
// Using the actual code (not just a bool) lets the caller propagate CANCELLED
// vs WRITE_ERROR correctly to MoQForwarder's soft/hard error classification.
class MoQCrossExecSubgroupFilter final
    : public moxygen::SubgroupConsumerFilter,
      public std::enable_shared_from_this<MoQCrossExecSubgroupFilter> {
public:
  explicit MoQCrossExecSubgroupFilter(folly::Executor* targetExec)
      : moxygen::SubgroupConsumerFilter(nullptr), targetExec_(targetExec) {}

  folly::Expected<folly::Unit, moxygen::MoQPublishError> object(
      uint64_t objectID,
      moxygen::Payload payload,
      moxygen::Extensions extensions = moxygen::noExtensions(),
      bool finSubgroup = false
  ) override;

  folly::Expected<folly::Unit, moxygen::MoQPublishError> beginObject(
      uint64_t objectID,
      uint64_t length,
      moxygen::Payload initialPayload,
      moxygen::Extensions extensions = moxygen::noExtensions()
  ) override;

  folly::Expected<moxygen::ObjectPublishStatus, moxygen::MoQPublishError>
  objectPayload(moxygen::Payload payload, bool finSubgroup = false) override;

  folly::Expected<folly::Unit, moxygen::MoQPublishError> endOfGroup(uint64_t endOfGroupObjectID
  ) override;

  folly::Expected<folly::Unit, moxygen::MoQPublishError>
  endOfTrackAndGroup(uint64_t endOfTrackObjectID) override;

  folly::Expected<folly::Unit, moxygen::MoQPublishError> endOfSubgroup() override;

  void reset(moxygen::ResetStreamErrorCode error) override;

  // Must be overridden: SubgroupConsumerFilter::checkpoint() calls
  // downstream_->checkpoint() but our inherited downstream_ is always null.
  void checkpoint() override;

  // Must be overridden: SubgroupConsumerFilter::awaitReadyToConsume() calls
  // downstream_->awaitReadyToConsume() but our inherited downstream_ is always null.
  folly::Expected<folly::SemiFuture<uint64_t>, moxygen::MoQPublishError>
  awaitReadyToConsume() override;

  // Called on targetExec_ by MoQCrossExecFilter::beginSubgroup to keep the
  // parent TrackConsumer (MoQForwarder) alive as long as this subgroup filter
  // exists. SubgroupForwarder holds a raw back-pointer to the parent forwarder;
  // without this, the forwarder can be freed while in-flight lambdas are still
  // executing reset/endOf* against it.
  void setKeepAlive(std::shared_ptr<moxygen::TrackConsumer> ka) {
    keepAlive_ = std::move(ka);
  }

private:
  folly::Executor* targetExec_;
  ObjectPayloadByteTracker payloadTracker_;
  // 0 = open; non-zero = MoQPublishError::Code value set by target thread.
  std::atomic<uint32_t> closeCode_{0};
  std::shared_ptr<moxygen::TrackConsumer> keepAlive_;
};

// Forwards all TrackConsumer calls to a target executor (fire-and-forget).
// All data methods return success immediately on the calling thread after
// checking closeCode_; the actual call runs asynchronously on targetExec.
// beginSubgroup() returns a MoQCrossExecSubgroupFilter that similarly defers
// to targetExec.
//
// Requires targetExec to be a FIFO executor so that delivery order is
// preserved without additional synchronization.
//
// For deferred use (e.g. publish()): construct with inner=nullptr, then call
// setDownstream() on targetExec_ before any data methods are enqueued. FIFO
// ordering guarantees setDownstream() runs before those lambdas execute.
class MoQCrossExecFilter final : public moxygen::TrackConsumerFilter,
                                 public std::enable_shared_from_this<MoQCrossExecFilter> {
public:
  MoQCrossExecFilter(folly::Executor* targetExec, std::shared_ptr<moxygen::TrackConsumer> inner)
      : moxygen::TrackConsumerFilter(std::move(inner)), targetExec_(targetExec) {}

  folly::Expected<folly::Unit, moxygen::MoQPublishError> setTrackAlias(moxygen::TrackAlias alias
  ) override;

  folly::Expected<std::shared_ptr<moxygen::SubgroupConsumer>, moxygen::MoQPublishError>
  beginSubgroup(
      uint64_t groupID,
      uint64_t subgroupID,
      moxygen::Priority priority,
      bool containsLastInGroup = false
  ) override;

  folly::Expected<folly::SemiFuture<folly::Unit>, moxygen::MoQPublishError>
  awaitStreamCredit() override;

  folly::Expected<folly::Unit, moxygen::MoQPublishError> objectStream(
      const moxygen::ObjectHeader& header,
      moxygen::Payload payload,
      bool lastInGroup = false
  ) override;

  folly::Expected<folly::Unit, moxygen::MoQPublishError>
  datagram(const moxygen::ObjectHeader& header, moxygen::Payload payload, bool lastInGroup = false)
      override;

  folly::Expected<folly::Unit, moxygen::MoQPublishError> publishDone(moxygen::PublishDone pubDone
  ) override;

private:
  folly::Executor* targetExec_;
  // 0 = open; non-zero = MoQPublishError::Code value set by target thread.
  std::atomic<uint32_t> closeCode_{0};
};

// Forwards all FetchConsumer calls to a target executor (fire-and-forget).
// All data methods return success immediately on the calling thread after
// checking closeCode_; the actual call runs asynchronously on targetExec.
//
// Requires targetExec to be a FIFO executor so that delivery order is
// preserved without additional synchronization.
class MoQFetchCrossExecFilter final
    : public moxygen::FetchConsumer,
      public std::enable_shared_from_this<MoQFetchCrossExecFilter> {
public:
  MoQFetchCrossExecFilter(
      folly::Executor* targetExec,
      std::shared_ptr<moxygen::FetchConsumer> downstream
  )
      : targetExec_(targetExec), downstream_(std::move(downstream)) {}

  folly::Expected<folly::Unit, moxygen::MoQPublishError> object(
      uint64_t groupID,
      uint64_t subgroupID,
      uint64_t objectID,
      moxygen::Payload payload,
      moxygen::Extensions extensions = moxygen::noExtensions(),
      bool finFetch = false,
      bool forwardingPreferenceIsDatagram = false
  ) override;

  folly::Expected<folly::Unit, moxygen::MoQPublishError> beginObject(
      uint64_t groupID,
      uint64_t subgroupID,
      uint64_t objectID,
      uint64_t length,
      moxygen::Payload initialPayload,
      moxygen::Extensions extensions = moxygen::noExtensions()
  ) override;

  folly::Expected<moxygen::ObjectPublishStatus, moxygen::MoQPublishError>
  objectPayload(moxygen::Payload payload, bool finSubgroup = false) override;

  folly::Expected<folly::Unit, moxygen::MoQPublishError>
  endOfGroup(uint64_t groupID, uint64_t subgroupID, uint64_t objectID, bool finFetch = false)
      override;

  folly::Expected<folly::Unit, moxygen::MoQPublishError>
  endOfTrackAndGroup(uint64_t groupID, uint64_t subgroupID, uint64_t objectID) override;

  folly::Expected<folly::Unit, moxygen::MoQPublishError> endOfFetch() override;

  void reset(moxygen::ResetStreamErrorCode error) override;

  folly::Expected<folly::SemiFuture<uint64_t>, moxygen::MoQPublishError>
  awaitReadyToConsume() override;

private:
  folly::Executor* targetExec_;
  std::shared_ptr<moxygen::FetchConsumer> downstream_;
  ObjectPayloadByteTracker payloadTracker_;
  // 0 = open; non-zero = MoQPublishError::Code value set by target thread.
  std::atomic<uint32_t> closeCode_{0};
};

} // namespace openmoq::moqx
