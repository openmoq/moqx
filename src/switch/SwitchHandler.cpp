/*
 * Copyright (c) Synamedia
 * SPDX-License-Identifier: Apache-2.0
 */

#include "SwitchHandler.h"
#include "../MoqxRelay.h"
#include "../MoqxSession.h"
#include "GroupStartObserver.h"
#include <moxygen/relay/MoQForwarder.h>
#include <folly/container/F14Set.h>
#include <folly/coro/Baton.h>

namespace openmoq::moqx {

SwitchHandler::SwitchHandler(
    std::shared_ptr<MoqxSession> session,
    moxygen::Switch sw,
    MoqxRelay& relay)
    : session_(std::move(session)), sw_(std::move(sw)), relay_(relay) {}

folly::coro::Task<void> SwitchHandler::run() {
  // ── VALIDATE ─────────────────────────────────────────────────────────────
  auto currentForwarder =
      relay_.getForwarder(sw_.currentSubscribeRequestID, session_.get());
  if (!currentForwarder) {
    co_await sendErrorPublishDone(
        moxygen::PublishDoneStatusCode::SUBSCRIPTION_ENDED,
        "currentSubscribeRequestID not found");
    co_return;
  }

  // ── SUBSCRIBE_TARGET ──────────────────────────────────────────────────────
  auto targetForwarder =
      co_await relay_.getOrSubscribeForwarder(sw_.targetTrackName);
  if (!targetForwarder) {
    co_await sendErrorPublishDone(
        moxygen::PublishDoneStatusCode::DOES_NOT_EXIST,
        "target track not available");
    co_return;
  }

  // ── OBSERVE_GSWITCH ───────────────────────────────────────────────────────
  // GroupStartObserver is added to the TARGET forwarder only (session_ is not
  // yet subscribed there). Current track availability is read synchronously
  // from currentForwarder->largest() during each evaluation.
  folly::coro::Baton gswitchFound;
  uint64_t gswitch = 0;
  folly::F14FastSet<uint64_t> availableTarget;

  auto tryFindGswitch = [&]() {
    auto currentLargeMaybe = currentForwarder->largest();
    auto targetLargeMaybe = targetForwarder->largest();
    if (!currentLargeMaybe || !targetLargeMaybe) {
      return;
    }

    uint64_t currentLarge = currentLargeMaybe->group;
    uint64_t targetLarge = targetLargeMaybe->group;

    for (uint64_t g = sw_.minimumSwitchingGroupID;
         g <= std::min(currentLarge, targetLarge);
         ++g) {
      bool ok = true;
      for (uint64_t gp = g; gp < targetLarge; ++gp) {
        if (gp <= currentLarge && !availableTarget.count(gp)) {
          ok = false;
          break;
        }
      }
      if (ok) {
        gswitch = g;
        gswitchFound.post();
        return;
      }
    }
  };

  // Pre-populate availableTarget from groups already in the target forwarder.
  // Without this, tryFindGswitch fires only on future beginSubgroup() callbacks,
  // missing already-available groups and potentially waiting indefinitely.
  if (auto targetLargeMaybe = targetForwarder->largest()) {
    for (uint64_t g = sw_.minimumSwitchingGroupID;
         g <= targetLargeMaybe->group;
         ++g) {
      availableTarget.insert(g);
    }
  }

  tryFindGswitch();
  if (!gswitchFound.ready()) {
    moxygen::SubscribeRequest obsSubReq;
    obsSubReq.fullTrackName = sw_.targetTrackName;
    obsSubReq.locType = moxygen::LocationType::AbsoluteStart;
    obsSubReq.start = moxygen::AbsoluteLocation{sw_.minimumSwitchingGroupID, 0};
    auto observer = std::make_shared<GroupStartObserver>(
        [&](uint64_t groupID) {
          availableTarget.insert(groupID);
          tryFindGswitch();
        });
    targetForwarder->addSubscriber(session_, obsSubReq, observer);

    // TODO: wrap with T_switch timeout (e.g. 5s) using co_withCancellation
    co_await gswitchFound;

    // Remove observer before re-adding session for live delivery.
    targetForwarder->removeSubscriber(session_, std::nullopt, "observer_done");
  }

  uint64_t liveEdge =
      targetForwarder->largest()
          .value_or(moxygen::AbsoluteLocation{gswitch, 0})
          .group;

  // ── CATCHUP + LIVE ────────────────────────────────────────────────────────
  // Spec requires continuing to forward from the current subscription during
  // SWITCH processing. CUT_OLD runs only AFTER publishForSwitch() succeeds so
  // that a publishForSwitch failure leaves the current subscription untouched.

  // Copy SWITCH params to pubReq: spec says SWITCH params become the complete
  // target-track PUBLISH param set.
  moxygen::PublishRequest pubReq;
  pubReq.fullTrackName = sw_.targetTrackName;
  pubReq.params = sw_.params;
  auto handle = relay_.getSubscriptionHandle(sw_.targetTrackName);

  auto switchResult = session_->publishForSwitch(
      pubReq, gswitch, liveEdge, sw_.currentSubscribeRequestID, handle);
  if (!switchResult) {
    co_return; // publishForSwitch failed — current subscription untouched
  }
  auto* writeHandle = switchResult->writeHandle; // null = draft < 17 fallback

  // ── CUT_OLD ───────────────────────────────────────────────────────────────
  // Reset open subgroup streams at group >= gswitch to purge buffered payload
  // from the QUIC send buffer before new-track objects arrive.
  auto sub = currentForwarder->getSubscriber(session_.get());
  if (sub) {
    for (auto& [id, consumer] : sub->subgroups) {
      if (id.group >= gswitch && consumer) {
        consumer->reset(moxygen::ResetStreamErrorCode::CANCELLED);
      }
    }
  }
  moxygen::PublishDone drainDone{
      .requestID = sw_.currentSubscribeRequestID,
      .statusCode = moxygen::PublishDoneStatusCode::SUBSCRIPTION_ENDED,
      .reasonPhrase = "switch_transition"};
  currentForwarder->drainSubscriber(session_, drainDone, "switch_handler");

  // Phase 1: FETCH catch-up [gswitch, liveEdge) on bidi write side.
  session_->writeCatchupToHandle(
      writeHandle, sw_.targetTrackName, gswitch, liveEdge, relay_.cache());

  // Phase 2: drain loop [liveEdge, drainPoint) via consumer → subgroup streams.
  uint64_t deliveredUpTo = liveEdge;
  if (switchResult->consumer && relay_.cache()) {
    while (true) {
      auto largestMaybe = targetForwarder->largest();
      uint64_t currentEdge =
          largestMaybe ? largestMaybe->group : deliveredUpTo;
      if (deliveredUpTo >= currentEdge) {
        break;
      }
      for (uint64_t g = deliveredUpTo; g < currentEdge; ++g) {
        auto subRes =
            switchResult->consumer->beginSubgroup(g, 0, /*priority=*/0);
        if (!subRes) {
          continue;
        }
        auto& subConsumer = subRes.value();
        for (uint64_t objID = 0;; ++objID) {
          auto* entry = relay_.cache()->getObject(
              sw_.targetTrackName, moxygen::AbsoluteLocation{g, objID});
          if (!entry) {
            break;
          }
          subConsumer->object(
              objID,
              entry->payload ? entry->payload->clone() : nullptr,
              entry->extensions,
              /*finSubgroup=*/false);
        }
      }
      deliveredUpTo = currentEdge;
    }

    // Live delivery from drainPoint onward.
    moxygen::SubscribeRequest liveSubReq;
    liveSubReq.fullTrackName = sw_.targetTrackName;
    liveSubReq.locType = moxygen::LocationType::AbsoluteStart;
    liveSubReq.start = moxygen::AbsoluteLocation{deliveredUpTo, 0};
    targetForwarder->addSubscriber(session_, liveSubReq, switchResult->consumer);
  }

  co_return;
}

folly::coro::Task<void> SwitchHandler::sendErrorPublishDone(
    moxygen::PublishDoneStatusCode statusCode,
    std::string reason) {
  // MoQSession::publish() rejects null handles. Reuse publishForSwitch() with
  // no catch-up to open a bidi stream, then immediately send publishDone.
  moxygen::PublishRequest pubReq;
  pubReq.fullTrackName = sw_.targetTrackName;
  auto handle = relay_.getSubscriptionHandle(sw_.targetTrackName);
  auto switchResult = session_->publishForSwitch(
      pubReq,
      /*switchingGroupID=*/0,
      /*liveEdgeGroupID=*/0,
      sw_.currentSubscribeRequestID,
      handle);
  if (switchResult && switchResult->consumer) {
    moxygen::PublishDone done{
        .requestID = sw_.currentSubscribeRequestID,
        .statusCode = statusCode,
        .reasonPhrase = std::move(reason)};
    switchResult->consumer->publishDone(std::move(done));
  }
  co_return;
}

} // namespace openmoq::moqx
