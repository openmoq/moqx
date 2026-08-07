/*
 * Copyright (c) OpenMOQ contributors.
 */

#include "relay/InitialTrackState.h"
#include "relay/NullConsumers.h"

#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>

using namespace testing;
using namespace moxygen;
using openmoq::moqx::InitialTrackState;

namespace {

const TrackNamespace kTestNs{{"test", "namespace"}};
const FullTrackName kFtn{kTestNs, "track1"};

constexpr uint64_t kExtType = 0xBEEF'0000;

Extensions extensionsWith(uint64_t value) {
  Extensions e;
  e.insertMutableExtension(Extension{kExtType, value});
  return e;
}

// addSubscriber only uses the session as a map key, so a null one is enough here.
std::shared_ptr<MoQForwarder::Subscriber> addSubscriber(MoQForwarder& forwarder) {
  SubscribeRequest req;
  req.fullTrackName = kFtn;
  req.requestID = RequestID(1);
  req.forward = true;
  return forwarder.addSubscriber(nullptr, req, std::make_shared<NullTrackConsumer>());
}

TEST(InitialTrackStateTest, CaptureRoundTripsPositionAndProperties) {
  MoQForwarder source(kFtn, AbsoluteLocation{7, 3});
  source.setExtensions(extensionsWith(42));

  auto state = InitialTrackState::capture(source);

  EXPECT_EQ(state.largest, (AbsoluteLocation{7, 3}));
  EXPECT_EQ(state.extensions.getIntExtension(kExtType), 42);
}

TEST(InitialTrackStateTest, CaptureOfFreshForwarderHasNoPosition) {
  MoQForwarder source(kFtn);
  auto state = InitialTrackState::capture(source);
  EXPECT_FALSE(state.largest.has_value());
}

TEST(InitialTrackStateTest, ApplyToForwarderSetsBothHalves) {
  MoQForwarder target(kFtn);
  InitialTrackState{AbsoluteLocation{4, 9}, extensionsWith(7)}.applyTo(target);

  EXPECT_EQ(target.largest(), (AbsoluteLocation{4, 9}));
  EXPECT_EQ(target.extensions().getIntExtension(kExtType), 7);
}

TEST(InitialTrackStateTest, ApplyWithoutPositionStillSetsProperties) {
  MoQForwarder target(kFtn);
  InitialTrackState{std::nullopt, extensionsWith(5)}.applyTo(target);

  EXPECT_FALSE(target.largest().has_value());
  EXPECT_EQ(target.extensions().getIntExtension(kExtType), 5);
}

// updateLargest is monotonic, so a stale capture cannot rewind a forwarder that has
// already advanced past it.
TEST(InitialTrackStateTest, ApplyDoesNotRegressAnAdvancedForwarder) {
  MoQForwarder target(kFtn, AbsoluteLocation{10, 0});
  InitialTrackState{AbsoluteLocation{2, 0}, Extensions{}}.applyTo(target);
  EXPECT_EQ(target.largest(), (AbsoluteLocation{10, 0}));
}

TEST(InitialTrackStateTest, CaptureThenApplyIsIdentity) {
  MoQForwarder source(kFtn, AbsoluteLocation{3, 1});
  source.setExtensions(extensionsWith(99));

  MoQForwarder target(kFtn);
  InitialTrackState::capture(source).applyTo(target);

  EXPECT_EQ(target.largest(), source.largest());
  EXPECT_EQ(target.extensions().getIntExtension(kExtType), 99);
}

TEST(InitialTrackStateTest, ApplyToSubscriberSetsPositionOnly) {
  MoQForwarder forwarder(kFtn);
  auto sub = addSubscriber(forwarder);
  ASSERT_NE(sub, nullptr);

  InitialTrackState{AbsoluteLocation{6, 2}, extensionsWith(11)}.applyTo(*sub);

  EXPECT_EQ(sub->subscribeOk().largest, (AbsoluteLocation{6, 2}));
  // The forwarder is the source of a subscriber's extensions; applyTo must not touch them.
  EXPECT_FALSE(sub->subscribeOk().extensions.getIntExtension(kExtType).has_value());
}

TEST(InitialTrackStateTest, ApplyToSubscriberWithoutPositionIsANoOp) {
  MoQForwarder forwarder(kFtn, AbsoluteLocation{1, 1});
  auto sub = addSubscriber(forwarder);
  ASSERT_NE(sub, nullptr);
  auto before = sub->subscribeOk().largest;

  InitialTrackState{std::nullopt, Extensions{}}.applyTo(*sub);

  EXPECT_EQ(sub->subscribeOk().largest, before);
}

} // namespace
