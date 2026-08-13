/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/CrossExecForwarderCallback.h"

#include <folly/executors/ManualExecutor.h>
#include <folly/portability/GTest.h>

#include <functional>
#include <optional>
#include <vector>

using namespace testing;
using namespace moxygen;
using namespace openmoq::moqx;

namespace {

const TrackNamespace kTestNs{{"test", "namespace"}};
const FullTrackName kFtn{kTestNs, "track1"};

struct RecordingTrackEventCallback : public TrackEventCallback {
  void onEmpty(const FullTrackName& ftn) override { onEmptyFtn = ftn; }

  void forwardChanged(const FullTrackName& ftn, bool forward) override {
    forwardChangedFtn = ftn;
    lastForward = forward;
  }

  void newGroupRequested(const FullTrackName& ftn, uint64_t group) override {
    newGroupFtn = ftn;
    lastGroup = group;
  }

  void onPublishDone(const FullTrackName& ftn) override { publishDoneFtn = ftn; }

  std::optional<FullTrackName> onEmptyFtn;
  std::optional<FullTrackName> forwardChangedFtn;
  std::optional<FullTrackName> newGroupFtn;
  std::optional<FullTrackName> publishDoneFtn;
  std::optional<bool> lastForward;
  std::optional<uint64_t> lastGroup;
};

class CrossExecForwarderCallbackTest : public ::testing::Test {
protected:
  void SetUp() override {
    downstream_ = std::make_shared<RecordingTrackEventCallback>();
    cb_ = std::make_shared<CrossExecForwarderCallback>(&target_, downstream_);
  }

  folly::ManualExecutor target_;
  std::shared_ptr<RecordingTrackEventCallback> downstream_;
  std::shared_ptr<CrossExecForwarderCallback> cb_;
};

// All four callbacks post the same lambda shape, so all four must capture nothing that keeps the
// forwarder alive.
TEST_F(CrossExecForwarderCallbackTest, QueuedEventsCarryTheNameNotTheForwarder) {
  using Fire = std::function<void(CrossExecForwarderCallback&, MoQForwarder*)>;
  const std::pair<const char*, Fire> cases[] = {
      {"onEmpty", [](CrossExecForwarderCallback& cb, MoQForwarder* f) { cb.onEmpty(f); }},
      {"forwardChanged",
       [](CrossExecForwarderCallback& cb, MoQForwarder* f) { cb.forwardChanged(f, true); }},
      {"newGroupRequested",
       [](CrossExecForwarderCallback& cb, MoQForwarder* f) { cb.newGroupRequested(f, 7); }},
      {"onPublishDone", [](CrossExecForwarderCallback& cb, MoQForwarder* f) { cb.onPublishDone(f); }
      }
  };

  // One callback per case, so no case can be perturbed by state a previous one left.
  std::vector<std::shared_ptr<CrossExecForwarderCallback>> callbacks;
  for (const auto& [name, fire] : cases) {
    SCOPED_TRACE(name);
    auto& cb =
        callbacks.emplace_back(std::make_shared<CrossExecForwarderCallback>(&target_, downstream_));
    auto forwarder = std::make_shared<MoQForwarder>(kFtn);
    std::weak_ptr<MoQForwarder> weak = forwarder;
    fire(*cb, forwarder.get());
    // The owner thread drops its last ref once the registry entry is vacated.
    forwarder.reset();
    EXPECT_TRUE(weak.expired()) << "queued event must not keep the forwarder alive";
  }

  EXPECT_EQ(target_.run(), std::size(cases));
  EXPECT_EQ(downstream_->onEmptyFtn, kFtn);
  EXPECT_EQ(downstream_->forwardChangedFtn, kFtn);
  EXPECT_EQ(downstream_->lastForward, true);
  EXPECT_EQ(downstream_->newGroupFtn, kFtn);
  EXPECT_EQ(downstream_->lastGroup, 7);
  EXPECT_EQ(downstream_->publishDoneFtn, kFtn);
}

// The name is read where the forwarder is alive, but the downstream call runs on the target.
TEST_F(CrossExecForwarderCallbackTest, DeliveryIsDeferredToTheTargetExecutor) {
  auto forwarder = std::make_shared<MoQForwarder>(kFtn);
  cb_->onEmpty(forwarder.get());

  EXPECT_FALSE(downstream_->onEmptyFtn.has_value()) << "must not run inline on the caller";
  EXPECT_EQ(target_.run(), 1);
  EXPECT_TRUE(downstream_->onEmptyFtn.has_value());
}

} // namespace
