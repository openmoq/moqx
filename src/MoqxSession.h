/*
 * Copyright (c) Synamedia
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <moxygen/MoQRelaySession.h>
#include <moxygen/MoQTypes.h>
#include <moxygen/relay/MoQCache.h>
#include <proxygen/lib/http/webtransport/WebTransport.h>
#include <memory>
#include <optional>

namespace openmoq::moqx {

class MoqxRelay;

struct SwitchPublishResult {
  // TrackConsumer for Phase 2 subgroup delivery after catch-up.
  std::shared_ptr<moxygen::TrackConsumer> consumer;
};

// Extends MoQRelaySession with SWITCH message dispatch and bidi PUBLISH support.
class MoqxSession : public moxygen::MoQRelaySession {
 public:
  using MoQRelaySession::MoQRelaySession;

  void setRelay(std::shared_ptr<MoqxRelay> relay);

  // MoQControlCodec::ControlCallback override
  void onSwitch(moxygen::Switch sw) override;

  // Open relay-initiated PUBLISH bidi stream for SWITCH response.
  // Writes PUBLISH (with SWITCH_TRANSITION param embedded) + FETCH_HEADER.
  // Returns {writeHandle, consumer} on success, nullopt on failure.
  std::optional<SwitchPublishResult> publishForSwitch(
      moxygen::PublishRequest pub,
      uint64_t switchingGroupID,
      uint64_t liveEdgeGroupID,
      moxygen::RequestID currentSubscribeRequestID,
      std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle);

  // Open a unidirectional FETCH stream and write FETCH_HEADER + catch-up
  // objects [gswitch, liveEdge) from cache. High priority (urgency=1).
  // moqFrameWriter_ is protected in MoQSession — must live here, not in
  // SwitchHandler.
  void writeCatchup(
      const moxygen::FullTrackName& trackName,
      uint64_t gswitch,
      uint64_t liveEdge,
      moxygen::RequestID currentSubscribeRequestID,
      moxygen::MoQCache* cache);

 private:
  std::shared_ptr<MoqxRelay> relay_;
};

} // namespace openmoq::moqx
