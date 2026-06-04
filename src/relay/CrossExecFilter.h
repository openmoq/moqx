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
#include <optional>

namespace openmoq::moqx {

// Non-virtual mixin holding the executor pointer, deep-copy flag, and
// deferred-error state shared by all CrossExec filter classes.
//
// deferredError_ is written on the target executor when downstream returns an
// error or is null, and read on the calling thread. It stores a
// MoQPublishError::Code cast to uint32_t; 0 means open. Storing the actual
// code (not just a bool) lets callers propagate CANCELLED vs WRITE_ERROR
// correctly to MoQForwarder's soft/hard error classification.
class CrossExecBase {
protected:
  CrossExecBase(folly::Executor* targetExec, bool deepCopyPayload)
      : targetExec_(targetExec), deepCopyPayload_(deepCopyPayload) {}

  std::optional<moxygen::MoQPublishError> loadDeferredError() const {
    if (auto code = deferredError_.load(std::memory_order_relaxed)) {
      return moxygen::MoQPublishError(static_cast<moxygen::MoQPublishError::Code>(code));
    }
    return std::nullopt;
  }

public:
  void storeDeferredError(moxygen::MoQPublishError::Code code) {
    deferredError_.store(static_cast<uint32_t>(code), std::memory_order_relaxed);
  }
  void storeDeferredError(const moxygen::MoQPublishError& err) { storeDeferredError(err.code); }

protected:
  folly::Executor* targetExec_;
  bool deepCopyPayload_;
  // 0 = open; non-zero = MoQPublishError::Code value set by target thread.
  std::atomic<uint32_t> deferredError_{0};
};

// CRTP mixin adding self-lifetime management to CrossExec filter classes.
//
// create() constructs the object via a PrivateTag-taking constructor (preventing
// direct make_shared by callers) then sets selfGuard_ = this so the object keeps
// itself alive until a terminal lambda on targetExec_ calls deactivate().
//
// PrivateTag is protected so only Derived::create() can construct objects this way.
//
// Lifetime protocol:
//   selfGuard_ keeps the object alive until a terminal lambda calls deactivate().
//   Non-terminal [this] lambdas that encounter an error call closeWithError(),
//   which stores the error, nulls downstream_ (via consumed), and enqueues a
//   no-op guard lambda that releases selfGuard_ after all already-queued [this]
//   lambdas have run (FIFO ordering guarantees the guard is always last among
//   lambdas enqueued before closeWithError was called).
//
//   Terminal [this] lambdas (finSubgroup/finFetch=true) call deactivate()
//   unconditionally, even when !downstream_, so the object is not leaked when the
//   source drops its ref after the terminal call without making another method call.
template <typename Derived> class CrossExecLifetime : public CrossExecBase {
protected:
  struct PrivateTag {};

  CrossExecLifetime(folly::Executor* exec, bool deepCopy) : CrossExecBase(exec, deepCopy) {}

public:
  // Called at the end of each terminal lambda (on targetExec_).
  // Drops the self-anchor; must be the last *this access in the lambda.
  void deactivate() { selfGuard_.reset(); }

  // Called by source methods from their loadDeferredError() early-return blocks.
  // Moves selfGuard_ into a no-op lambda so the release lands after all [this]
  // lambdas already enqueued by this (sequential) source thread. Idempotent.
  void enqueueDeactivate() {
    if (selfGuard_) {
      targetExec_->add([guard = std::move(selfGuard_)]() {});
    }
  }

  // Store error and null downstream_ (via consumed). The source thread schedules
  // the selfGuard_ release via enqueueDeactivate() in its loadDeferredError() block.
  // Pass std::move(downstream_) as consumed to null the member before returning.
  template <typename T = void>
  void
  closeWithError(moxygen::MoQPublishError::Code code, std::shared_ptr<T> /*consumed*/ = nullptr) {
    storeDeferredError(code);
  }
  template <typename T = void>
  void closeWithError(const moxygen::MoQPublishError& err, std::shared_ptr<T> consumed = nullptr) {
    closeWithError(err.code, std::move(consumed));
  }

protected:
  // Set by create(), cleared by deactivate() or moved out by enqueueDeactivate().
  std::shared_ptr<Derived> selfGuard_;
};

// Forwards all SubgroupConsumer calls to a target executor (fire-and-forget).
// downstream_ starts null and is populated by CrossExecFilter::beginSubgroup()
// on the target executor. FIFO ordering guarantees it is set before any
// object/endOf* calls enqueued afterward execute.
class CrossExecSubgroupFilter final : public moxygen::SubgroupConsumerFilter,
                                      public std::enable_shared_from_this<CrossExecSubgroupFilter>,
                                      public CrossExecLifetime<CrossExecSubgroupFilter> {
public:
  static std::shared_ptr<CrossExecSubgroupFilter>
  create(folly::Executor* targetExec, bool deepCopyPayload = true) {
    auto f = std::make_shared<CrossExecSubgroupFilter>(PrivateTag{}, targetExec, deepCopyPayload);
    f->selfGuard_ = f;
    return f;
  }

  CrossExecSubgroupFilter(PrivateTag, folly::Executor* targetExec, bool deepCopyPayload = true)
      : moxygen::SubgroupConsumerFilter(nullptr),
        CrossExecLifetime<CrossExecSubgroupFilter>(targetExec, deepCopyPayload) {}

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

  // Called on targetExec_ by CrossExecFilter::beginSubgroup to keep the
  // parent TrackConsumer (MoQForwarder) alive as long as this subgroup filter
  // exists. SubgroupForwarder holds a raw back-pointer to the parent forwarder;
  // without this, the forwarder can be freed while in-flight lambdas are still
  // executing reset/endOf* against it.
  void setKeepAlive(std::shared_ptr<moxygen::TrackConsumer> ka) { keepAlive_ = std::move(ka); }

private:
  ObjectPayloadByteTracker payloadTracker_;
  std::shared_ptr<moxygen::TrackConsumer> keepAlive_;
};

// Forwards all TrackConsumer calls to a target executor (fire-and-forget).
// All data methods return success immediately on the calling thread after
// checking deferredError_; the actual call runs asynchronously on targetExec.
// beginSubgroup() returns a CrossExecSubgroupFilter that similarly defers
// to targetExec.
//
// Requires targetExec to be a FIFO executor so that delivery order is
// preserved without additional synchronization.
//
// For deferred use (e.g. publish()): construct with inner=nullptr, then call
// setDownstream() on targetExec_ before any data methods are enqueued. FIFO
// ordering guarantees setDownstream() runs before those lambdas execute.
class CrossExecFilter final : public moxygen::TrackConsumerFilter,
                              public std::enable_shared_from_this<CrossExecFilter>,
                              public CrossExecBase {
public:
  CrossExecFilter(
      folly::Executor* targetExec,
      std::shared_ptr<moxygen::TrackConsumer> inner,
      bool deepCopyPayload = true
  )
      : moxygen::TrackConsumerFilter(std::move(inner)), CrossExecBase(targetExec, deepCopyPayload) {
  }

  folly::Expected<folly::Unit, moxygen::MoQPublishError> setTrackAlias(moxygen::TrackAlias alias
  ) override;

  folly::Expected<std::shared_ptr<moxygen::SubgroupConsumer>, moxygen::MoQPublishError>
  beginSubgroup(
      uint64_t groupID,
      uint64_t subgroupID,
      moxygen::Priority priority,
      moxygen::BeginSubgroupOptions options = {}
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

  uint64_t objectStreamErrors() const {
    return objectStreamErrors_.load(std::memory_order_relaxed);
  }
  uint64_t datagramErrors() const { return datagramErrors_.load(std::memory_order_relaxed); }

private:
  std::atomic<uint64_t> objectStreamErrors_{0};
  std::atomic<uint64_t> datagramErrors_{0};
};

// Forwards all FetchConsumer calls to a target executor (fire-and-forget).
// All data methods return success immediately on the calling thread after
// checking deferredError_; the actual call runs asynchronously on targetExec.
//
// Requires targetExec to be a FIFO executor so that delivery order is
// preserved without additional synchronization.
class FetchCrossExecFilter final : public moxygen::FetchConsumer,
                                   public std::enable_shared_from_this<FetchCrossExecFilter>,
                                   public CrossExecLifetime<FetchCrossExecFilter> {
public:
  static std::shared_ptr<FetchCrossExecFilter> create(
      folly::Executor* targetExec,
      std::shared_ptr<moxygen::FetchConsumer> downstream,
      bool deepCopyPayload = false
  ) {
    auto f = std::make_shared<FetchCrossExecFilter>(
        PrivateTag{},
        targetExec,
        std::move(downstream),
        deepCopyPayload
    );
    f->selfGuard_ = f;
    return f;
  }

  FetchCrossExecFilter(
      PrivateTag,
      folly::Executor* targetExec,
      std::shared_ptr<moxygen::FetchConsumer> downstream,
      bool deepCopyPayload = false
  )
      : CrossExecLifetime<FetchCrossExecFilter>(targetExec, deepCopyPayload),
        downstream_(std::move(downstream)) {}

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
  objectPayload(moxygen::Payload payload, bool finFetch = false) override;

  folly::Expected<folly::Unit, moxygen::MoQPublishError>
  endOfGroup(uint64_t groupID, uint64_t subgroupID, uint64_t objectID, bool finFetch = false)
      override;

  folly::Expected<folly::Unit, moxygen::MoQPublishError>
  endOfTrackAndGroup(uint64_t groupID, uint64_t subgroupID, uint64_t objectID) override;

  folly::Expected<folly::Unit, moxygen::MoQPublishError> endOfFetch() override;

  void reset(moxygen::ResetStreamErrorCode error) override;

  void checkpoint() override;

  folly::Expected<folly::Unit, moxygen::MoQPublishError>
  endOfUnknownRange(uint64_t groupID, uint64_t objectID, bool finFetch = false) override;

  folly::Expected<folly::SemiFuture<uint64_t>, moxygen::MoQPublishError>
  awaitReadyToConsume() override;

private:
  std::shared_ptr<moxygen::FetchConsumer> downstream_;
  ObjectPayloadByteTracker payloadTracker_;
};

} // namespace openmoq::moqx
