/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "peering/PeeringObject.h"

#include <moxygen/MoQTypes.h>

#include <memory>

namespace openmoq::moqx {

// Local PUBLISH and PUBLISH_NAMESPACE cross this into moq-peering. The peering
// participant calls it on the relay executor. Implementations must not re-enter
// the relay.
class PeeringAnnouncer {
public:
  virtual ~PeeringAnnouncer() = default;

  virtual void announceNamespace(const moxygen::TrackNamespace& ns) = 0;
  virtual void withdrawNamespace(const moxygen::TrackNamespace& ns) = 0;
  virtual void announceTrack(const moxygen::FullTrackName& ftn) = 0;
  virtual void withdrawTrack(const moxygen::FullTrackName& ftn) = 0;

  // A locally published object a peer has subscribed to. The implementation
  // encodes it as a MoQT subgroup object or datagram before SendData. The
  // payload is capped at 1MiB. Implementations must not re-enter the relay.
  virtual void sendObject(const moxygen::FullTrackName& /*ftn*/, PeeringObject /*object*/) {}

  // The local subgroup has finished. reset closes the peering stream with a reset.
  virtual void endSubgroup(
      const moxygen::FullTrackName& /*ftn*/,
      uint64_t /*group*/,
      uint64_t /*subgroup*/,
      bool /*reset*/
  ) {}

  // Local subscribers want a track a peer announced.
  virtual void subscribeTrack(const moxygen::FullTrackName& /*ftn*/) {}
  virtual void unsubscribeTrack(const moxygen::FullTrackName& /*ftn*/) {}

  // Session id of publishes injected from a peer, so the relay does not
  // announce those names back out.
  virtual moxygen::SessionId sessionId() const = 0;
};

} // namespace openmoq::moqx
