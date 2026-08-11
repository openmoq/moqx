/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/relay/MoQForwarder.h>

#include <optional>

namespace openmoq::moqx {

// The position and properties a subscriber must observe before it can be told about a
// track — what SUBSCRIBE_OK and PUBLISH carry. A forwarder that has not had one applied
// is not attachable: a subscriber reading its largest first gets a value the client
// interprets as a track restart.
struct InitialTrackState {
  std::optional<moxygen::AbsoluteLocation> largest;
  moxygen::Extensions extensions;

  // A point-in-time copy; the publisher may advance past it before it is applied.
  static InitialTrackState capture(moxygen::MoQForwarder& forwarder) {
    return {forwarder.largest(), forwarder.extensions()};
  }

  void applyTo(moxygen::MoQForwarder& forwarder) const {
    forwarder.setExtensions(extensions);
    if (largest) {
      forwarder.updateLargest(largest->group, largest->object);
    }
  }

  // Extensions reach a subscriber through its forwarder; only the position is per-subscriber.
  void applyTo(moxygen::MoQForwarder::Subscriber& subscriber) const {
    if (largest) {
      subscriber.updateLargest(*largest);
    }
  }
};

} // namespace openmoq::moqx
