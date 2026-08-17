/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/relay/MoQForwarder.h>

namespace openmoq::moqx {

// TrackName based version of MoQForwarder::Callback.  Identifies the forwarder by track
// name in cases where the forwarder itself may not be available.
class TrackEventCallback {
public:
  virtual ~TrackEventCallback() = default;

  virtual void onEmpty(const moxygen::FullTrackName& ftn) = 0;
  virtual void forwardChanged(const moxygen::FullTrackName& ftn, bool forward) = 0;
  virtual void newGroupRequested(const moxygen::FullTrackName& ftn, uint64_t group) = 0;
  virtual void onPublishDone(const moxygen::FullTrackName& ftn) = 0;
};

} // namespace openmoq::moqx
