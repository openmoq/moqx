/*
 * Copyright (c) Synamedia
 * SPDX-License-Identifier: Apache-2.0
 */

#include "MoqxSession.h"
#include "MoqxRelay.h"
#include "switch/SwitchTypes.h"
#include <moxygen/MoQFramer.h>

namespace openmoq::moqx {

void MoqxSession::setRelay(std::shared_ptr<MoqxRelay> relay) {
  relay_ = std::move(relay);
}

void MoqxSession::onSwitch(moxygen::Switch sw) {
  if (relay_) {
    // shared_from_this() returns shared_ptr<MoQSession> (enable_shared_from_this
    // is on the base class). Cast to MoqxSession before passing to handleSwitch.
    relay_->handleSwitch(
        std::static_pointer_cast<MoqxSession>(shared_from_this()),
        std::move(sw));
  }
}

std::optional<SwitchPublishResult> MoqxSession::publishForSwitch(
    moxygen::PublishRequest pub,
    uint64_t switchingGroupID,
    uint64_t liveEdgeGroupID,
    moxygen::RequestID currentSubscribeRequestID,
    std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle) {

  // Step 1: encode SWITCH_TRANSITION value bytes and insert into pub.params.
  // Parameters::params_ is private — must use insertParam(), not push_back().
  // Parameters::isParamAllowed() returns true for unknown keys (backward compat),
  // so kSwitchTransitionParamKey (0xFF01) will be accepted.
  // writeTrackRequestParams() handles delta-encoding for v16+ automatically.
  folly::IOBufQueue valBuf{folly::IOBufQueue::cacheChainLength()};
  folly::io::QueueAppender va(&valBuf, 16);
  quic::encodeQuicInteger(switchingGroupID, [&](auto b) { va.push(b); });
  quic::encodeQuicInteger(liveEdgeGroupID, [&](auto b) { va.push(b); });
  auto valStr = valBuf.move()->moveToFbString().toStdString();
  pub.params.insertParam(moxygen::Parameter{kSwitchTransitionParamKey, valStr});

  // Step 2: assign requestID/trackAlias.
  pub.requestID = getNextRequestID();
  pub.trackAlias = moxygen::TrackAlias(pub.requestID.value);

  // Step 3: register TrackPublisherImpl BEFORE opening the bidi stream.
  // onPublishOk() looks up pendingRequests_ by requestID immediately when
  // PUBLISH_OK arrives. Registration must precede the wire send.
  pub.forward = true;
  auto consumer = registerPublishConsumerForSwitch(pub, handle);

  // Step 4: write PUBLISH frame and open bidi stream (draft >= 17).
  folly::IOBufQueue writeBuf{folly::IOBufQueue::cacheChainLength()};
  auto writeRes = moqFrameWriter_.writePublish(writeBuf, pub);
  if (!writeRes) {
    return std::nullopt;
  }

  // sendRequest() returns Expected<StreamWriteHandle*, string>.
  auto sendRes = sendRequest(
      writeBuf,
      {moxygen::FrameType::PUBLISH_OK, moxygen::FrameType::PUBLISH_ERROR},
      pub.requestID,
      /*minBidiDraftVersion=*/17);
  if (sendRes.hasError()) {
    return std::nullopt;
  }
  auto* writeHandle = sendRes.value(); // nullptr = control stream path (draft < 17)

  // Step 5: write FETCH_HEADER on the bidi write side — catch-up section begins here.
  if (writeHandle) {
    folly::IOBufQueue fetchHeaderBuf{folly::IOBufQueue::cacheChainLength()};
    moqFrameWriter_.writeFetchHeader(fetchHeaderBuf, currentSubscribeRequestID);
    writeHandle->writeStreamData(fetchHeaderBuf.move(), /*eof=*/false);
  }

  return SwitchPublishResult{writeHandle, consumer};
}

void MoqxSession::writeCatchupToHandle(
    proxygen::WebTransport::StreamWriteHandle* writeHandle,
    const moxygen::FullTrackName& trackName,
    uint64_t gswitch,
    uint64_t liveEdge,
    moxygen::MoQCache* cache) {
  // moqFrameWriter_ is protected in MoQSession — accessible here (MoqxSession
  // IS-A MoQSession) but not from SwitchHandler (a separate class).
  if (!writeHandle || !cache) {
    return;
  }
  for (uint64_t g = gswitch; g < liveEdge; ++g) {
    for (uint64_t objID = 0;; ++objID) {
      auto* entry =
          cache->getObject(trackName, moxygen::AbsoluteLocation{g, objID});
      if (!entry) {
        break; // nullptr = not cached or gap; treat as end of group for POC
      }
      // writeStreamObject() CHECKs that if objectPayload is non-empty, length
      // must be set. Set length = payloadSize for NORMAL objects.
      std::optional<uint64_t> objLen = std::nullopt;
      if (entry->status == moxygen::ObjectStatus::NORMAL &&
          entry->payloadSize > 0) {
        objLen = entry->payloadSize;
      }
      moxygen::ObjectHeader hdr{
          .group = g,
          .subgroup = entry->subgroup,
          .id = objID,
          .priority = std::nullopt,
          .status = entry->status,
          .extensions = entry->extensions,
          .length = objLen};
      folly::IOBufQueue objBuf{folly::IOBufQueue::cacheChainLength()};
      moqFrameWriter_.writeStreamObject(
          objBuf,
          moxygen::StreamType::FETCH_HEADER,
          hdr,
          entry->payload ? entry->payload->clone() : nullptr,
          entry->forwardingPreferenceIsDatagram);
      writeHandle->writeStreamData(objBuf.move(), /*eof=*/false);
    }
  }
  // Bidi write side remains open — FETCH section ends implicitly at liveEdge
  // via SWITCH_TRANSITION. No FIN issued here.
}

} // namespace openmoq::moqx
