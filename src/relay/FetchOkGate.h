/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <optional>
#include <utility>

#include <moxygen/MoQConsumers.h>

namespace openmoq::moqx {

// Pass-through FetchConsumer that holds back the fetch's fin until the caller
// has validated the upstream FETCH_OK: moxygen can deliver all fetch data and
// the FIN before FETCH_OK, and a rejected FETCH_OK must end downstream with
// only a reset. Must run on the caller's executor.
class FetchOkGate : public moxygen::FetchConsumer {
public:
  explicit FetchOkGate(std::shared_ptr<moxygen::FetchConsumer> downstream)
      : downstream_(std::move(downstream)) {}

  // FETCH_OK accepted: replay a held terminal and stop gating.
  void accept() {
    accepted_ = true;
    if (auto eot = std::exchange(pendingEndOfTrack_, std::nullopt)) {
      (void)downstream_->endOfTrackAndGroup(eot->groupID, eot->subgroupID, eot->objectID);
    } else if (std::exchange(pendingEndOfFetch_, false)) {
      (void)downstream_->endOfFetch();
    }
  }

  bool hasPendingTerminal() const { return pendingEndOfFetch_ || pendingEndOfTrack_; }

  bool wasReset() const { return wasReset_; }

  folly::Expected<folly::Unit, moxygen::MoQPublishError> object(
      uint64_t groupID,
      uint64_t subgroupID,
      uint64_t objectID,
      moxygen::Payload payload,
      moxygen::Extensions extensions,
      bool finFetch,
      bool forwardingPreferenceIsDatagram
  ) override {
    return downstream_->object(
        groupID,
        subgroupID,
        objectID,
        std::move(payload),
        std::move(extensions),
        gateFin(finFetch),
        forwardingPreferenceIsDatagram
    );
  }

  void checkpoint() override { downstream_->checkpoint(); }

  folly::Expected<folly::Unit, moxygen::MoQPublishError> beginObject(
      uint64_t groupID,
      uint64_t subgroupID,
      uint64_t objectID,
      uint64_t length,
      moxygen::Payload initialPayload,
      moxygen::Extensions extensions
  ) override {
    return downstream_->beginObject(
        groupID,
        subgroupID,
        objectID,
        length,
        std::move(initialPayload),
        std::move(extensions)
    );
  }

  folly::Expected<moxygen::ObjectPublishStatus, moxygen::MoQPublishError>
  objectPayload(moxygen::Payload payload, bool finFetch) override {
    return downstream_->objectPayload(std::move(payload), gateFin(finFetch));
  }

  folly::Expected<folly::Unit, moxygen::MoQPublishError>
  endOfGroup(uint64_t groupID, uint64_t subgroupID, uint64_t objectID, bool finFetch) override {
    return downstream_->endOfGroup(groupID, subgroupID, objectID, gateFin(finFetch));
  }

  folly::Expected<folly::Unit, moxygen::MoQPublishError>
  endOfTrackAndGroup(uint64_t groupID, uint64_t subgroupID, uint64_t objectID) override {
    // Implies endOfFetch, so it is a terminal like a fin.
    if (!accepted_) {
      pendingEndOfTrack_ = EndOfTrack{groupID, subgroupID, objectID};
      return folly::unit;
    }
    return downstream_->endOfTrackAndGroup(groupID, subgroupID, objectID);
  }

  folly::Expected<folly::Unit, moxygen::MoQPublishError> endOfFetch() override {
    if (!gateFin(true)) {
      return folly::unit;
    }
    return downstream_->endOfFetch();
  }

  void reset(moxygen::ResetStreamErrorCode error) override {
    if (!std::exchange(wasReset_, true)) {
      downstream_->reset(error);
    }
  }

  void goaway(moxygen::Goaway goaway) override { downstream_->goaway(std::move(goaway)); }

  folly::Expected<folly::SemiFuture<uint64_t>, moxygen::MoQPublishError>
  awaitReadyToConsume() override {
    return downstream_->awaitReadyToConsume();
  }

  folly::Expected<folly::Unit, moxygen::MoQPublishError>
  endOfUnknownRange(uint64_t groupID, uint64_t objectID, bool finFetch) override {
    return downstream_->endOfUnknownRange(groupID, objectID, gateFin(finFetch));
  }

private:
  bool gateFin(bool fin) {
    if (fin && !accepted_) {
      pendingEndOfFetch_ = true;
      return false;
    }
    return fin;
  }

  std::shared_ptr<moxygen::FetchConsumer> downstream_;
  bool accepted_{false};
  bool pendingEndOfFetch_{false};
  struct EndOfTrack {
    uint64_t groupID;
    uint64_t subgroupID;
    uint64_t objectID;
  };
  std::optional<EndOfTrack> pendingEndOfTrack_;
  bool wasReset_{false};
};

} // namespace openmoq::moqx
