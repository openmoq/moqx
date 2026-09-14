/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/MoQConsumers.h>

#include <folly/container/F14Map.h>
#include <folly/hash/Hash.h>
#include <folly/logging/xlog.h>

#include <memory>
#include <optional>
#include <utility>

namespace openmoq::moqx {

// Writes FETCH-shaped output into a SUBSCRIBE-shaped consumer.
//
// The cache serves objects through a FetchConsumer, which is flat: every call
// carries its own (group, subgroup, object). A subscriber is fed through a
// TrackConsumer, where objects arrive on a SubgroupConsumer opened per
// (group, subgroup). This adapter keeps one open SubgroupConsumer per pair and
// closes it when the fetch says the group ended, so a cache replay reaches a
// live subscriber on ordinary subgroup streams.
//
// Why this is safe to interleave with live delivery: moxygen clamps a past
// AbsoluteStart forward to largest + 1 (see toSubscribeRange in
// moxygen/MoQLocation.h), so the live subscription starts exactly one object
// after the replay range ends. The two are complementary — no overlap, no gap —
// and MoQT groups are independent streams, so a replayed group arriving after a
// live one is reassembled by group id rather than by arrival order.
//
// Single-threaded: construct and drive it on the executor that owns the
// TrackConsumer it wraps. Errors are swallowed rather than propagated because a
// replay is best-effort; the subscriber's live subscription is already in place
// and must not be failed because a retained object could not be re-sent.
class FetchToTrackConsumer : public moxygen::FetchConsumer {
 public:
  explicit FetchToTrackConsumer(std::shared_ptr<moxygen::TrackConsumer> track, uint8_t priority)
      : track_(std::move(track)), priority_(priority) {}

  ~FetchToTrackConsumer() override {
    // Never leave a stream half-open: a subscriber would wait on it forever.
    closeAll();
  }

  folly::Expected<folly::Unit, moxygen::MoQPublishError> object(
      uint64_t groupID,
      uint64_t subgroupID,
      uint64_t objectID,
      moxygen::Payload payload,
      moxygen::Extensions extensions = moxygen::noExtensions(),
      bool finFetch = false,
      bool /*forwardingPreferenceIsDatagram*/ = false
  ) override {
    auto subgroup = openSubgroup(groupID, subgroupID);
    if (!subgroup) {
      return folly::unit;
    }
    auto res = subgroup->object(objectID, std::move(payload), std::move(extensions));
    if (res.hasError()) {
      dropSubgroup(groupID, subgroupID);
    }
    if (finFetch) {
      closeAll();
    }
    return folly::unit;
  }

  folly::Expected<folly::Unit, moxygen::MoQPublishError> beginObject(
      uint64_t groupID,
      uint64_t subgroupID,
      uint64_t objectID,
      uint64_t length,
      moxygen::Payload initialPayload,
      moxygen::Extensions extensions = moxygen::noExtensions()
  ) override {
    auto subgroup = openSubgroup(groupID, subgroupID);
    if (!subgroup) {
      return folly::unit;
    }
    // objectPayload() carries no coordinates, so remember where the parts go.
    streaming_ = SubgroupKey{groupID, subgroupID};
    auto res = subgroup->beginObject(objectID, length, std::move(initialPayload), std::move(extensions));
    if (res.hasError()) {
      dropSubgroup(groupID, subgroupID);
      streaming_.reset();
    }
    return folly::unit;
  }

  folly::Expected<moxygen::ObjectPublishStatus, moxygen::MoQPublishError> objectPayload(
      moxygen::Payload payload,
      bool /*finSubgroup*/ = false
  ) override {
    if (!streaming_) {
      return moxygen::ObjectPublishStatus::DONE;
    }
    auto it = subgroups_.find(*streaming_);
    if (it == subgroups_.end()) {
      return moxygen::ObjectPublishStatus::DONE;
    }
    auto res = it->second->objectPayload(std::move(payload));
    if (res.hasError()) {
      dropSubgroup(streaming_->group, streaming_->subgroup);
      streaming_.reset();
      return moxygen::ObjectPublishStatus::DONE;
    }
    if (*res == moxygen::ObjectPublishStatus::DONE) {
      streaming_.reset();
    }
    return *res;
  }

  folly::Expected<folly::Unit, moxygen::MoQPublishError> endOfGroup(
      uint64_t groupID,
      uint64_t subgroupID,
      uint64_t objectID,
      bool finFetch = false
  ) override {
    auto it = subgroups_.find(SubgroupKey{groupID, subgroupID});
    if (it != subgroups_.end()) {
      it->second->endOfGroup(objectID);
      subgroups_.erase(it);
    }
    if (finFetch) {
      closeAll();
    }
    return folly::unit;
  }

  folly::Expected<folly::Unit, moxygen::MoQPublishError>
  endOfTrackAndGroup(uint64_t groupID, uint64_t subgroupID, uint64_t objectID) override {
    auto it = subgroups_.find(SubgroupKey{groupID, subgroupID});
    if (it != subgroups_.end()) {
      it->second->endOfTrackAndGroup(objectID);
      subgroups_.erase(it);
    }
    return folly::unit;
  }

  folly::Expected<folly::Unit, moxygen::MoQPublishError> endOfFetch() override {
    closeAll();
    return folly::unit;
  }

  void reset(moxygen::ResetStreamErrorCode error) override {
    for (auto& [key, subgroup] : subgroups_) {
      subgroup->reset(error);
    }
    subgroups_.clear();
    streaming_.reset();
  }

  // The replay is bounded by what the cache holds, so there is nothing to wait
  // for; a subscriber that runs out of stream credit is handled by the live
  // subscription it already has.
  folly::Expected<folly::SemiFuture<uint64_t>, moxygen::MoQPublishError>
  awaitReadyToConsume() override {
    return folly::makeSemiFuture<uint64_t>(0);
  }

 private:
  struct SubgroupKey {
    uint64_t group;
    uint64_t subgroup;
    bool operator==(const SubgroupKey& other) const {
      return group == other.group && subgroup == other.subgroup;
    }
  };
  struct SubgroupKeyHash {
    size_t operator()(const SubgroupKey& key) const {
      return folly::hash::hash_combine(key.group, key.subgroup);
    }
  };

  std::shared_ptr<moxygen::SubgroupConsumer> openSubgroup(uint64_t groupID, uint64_t subgroupID) {
    SubgroupKey key{groupID, subgroupID};
    auto it = subgroups_.find(key);
    if (it != subgroups_.end()) {
      return it->second;
    }
    auto res = track_->beginSubgroup(groupID, subgroupID, priority_);
    if (res.hasError()) {
      XLOG(DBG2) << "cache replay could not open subgroup g=" << groupID << " sg=" << subgroupID
                 << " err=" << res.error().msg;
      return nullptr;
    }
    return subgroups_.emplace(key, std::move(res.value())).first->second;
  }

  void dropSubgroup(uint64_t groupID, uint64_t subgroupID) {
    subgroups_.erase(SubgroupKey{groupID, subgroupID});
  }

  void closeAll() {
    for (auto& [key, subgroup] : subgroups_) {
      subgroup->endOfSubgroup();
    }
    subgroups_.clear();
    streaming_.reset();
  }

  std::shared_ptr<moxygen::TrackConsumer> track_;
  uint8_t priority_;
  folly::F14FastMap<SubgroupKey, std::shared_ptr<moxygen::SubgroupConsumer>, SubgroupKeyHash>
      subgroups_;
  std::optional<SubgroupKey> streaming_;
};

} // namespace openmoq::moqx
