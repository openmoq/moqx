/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/MoQConsumers.h>
#include <moxygen/Publisher.h>

namespace openmoq::moqx {

// Tracks multi-part object byte delivery so objectPayload() can return the
// correct ObjectPublishStatus without depending on finSubgroup.
//
// Usage:
//   Call beginObject(length) from beginObject().
//   Call consume(payload) from objectPayload() before moving the payload;
//   the returned status is the value to return to the caller.
struct ObjectPayloadByteTracker {
  void beginObject(uint64_t length, const moxygen::Payload& initialPayload = nullptr) {
    remaining_ = length;
    if (initialPayload) {
      remaining_ -= std::min(remaining_, initialPayload->computeChainDataLength());
    }
  }

  moxygen::ObjectPublishStatus consume(const moxygen::Payload& payload) {
    if (payload && remaining_ > 0) {
      auto n = payload->computeChainDataLength();
      remaining_ -= std::min(remaining_, n);
    }
    return remaining_ == 0 ? moxygen::ObjectPublishStatus::DONE
                           : moxygen::ObjectPublishStatus::IN_PROGRESS;
  }

private:
  uint64_t remaining_{0};
};

} // namespace openmoq::moqx
