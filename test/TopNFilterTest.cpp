/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/TopNFilter.h"

#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <moxygen/test/Mocks.h>

#include <thread>

using namespace testing;
using namespace moxygen;
using namespace openmoq::moqx;

namespace {

const TrackNamespace kNs{{"test"}};
const FullTrackName kFtn{kNs, "track1"};
constexpr uint64_t kPropA = 0x100;
constexpr uint64_t kPropB = 0x200;

// Build an Extensions object with a single integer extension
Extensions makeExt(uint64_t type, uint64_t value) {
  Extensions ext;
  ext.insertMutableExtension(Extension{type, value});
  return ext;
}

// A TopNFilter wired to a NiceMock downstream for isolation
class TopNFilterTest : public ::testing::Test {
protected:
  void SetUp() override {
    downstream_ = std::make_shared<NiceMock<MockTrackConsumer>>();
    // downstream objectStream / publishDone always succeed
    ON_CALL(*downstream_, objectStream(_, _, _))
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
    ON_CALL(*downstream_, datagram(_, _, _))
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
    ON_CALL(*downstream_, publishDone(_))
        .WillByDefault(Return(folly::makeExpected<MoQPublishError>(folly::unit)));

    filter_ = std::make_shared<TopNFilter>(kFtn, downstream_);
  }

  std::shared_ptr<NiceMock<MockTrackConsumer>> downstream_;
  std::shared_ptr<TopNFilter> filter_;
};

// ---------------------------------------------------------------------------
// registerObserver / removeObserver
// ---------------------------------------------------------------------------

TEST_F(TopNFilterTest, NoObserversInitially) {
  EXPECT_FALSE(filter_->hasObservers());
}

TEST_F(TopNFilterTest, RegisterObserverSetsHasObservers) {
  filter_->registerObserver(kPropA, PropertyObserver{});
  EXPECT_TRUE(filter_->hasObservers());
}

TEST_F(TopNFilterTest, RemoveObserverClearsHasObservers) {
  filter_->registerObserver(kPropA, PropertyObserver{});
  filter_->removeObserver(kPropA);
  EXPECT_FALSE(filter_->hasObservers());
}

TEST_F(TopNFilterTest, RemoveNonExistentObserverIsNoop) {
  // Should not crash
  filter_->removeObserver(kPropA);
  EXPECT_FALSE(filter_->hasObservers());
}

// Only one observer per property type; second registration silently overwrites first.
TEST_F(TopNFilterTest, SecondRegisterOverwritesFirst) {
  int calls1 = 0;
  int calls2 = 0;
  filter_->registerObserver(kPropA, PropertyObserver{.onValueChanged = [&](uint64_t) {
                              calls1++;
                            }});
  filter_->registerObserver(kPropA, PropertyObserver{.onValueChanged = [&](uint64_t) {
                              calls2++;
                            }});

  filter_->checkProperties(makeExt(kPropA, 42));
  EXPECT_EQ(calls1, 0); // first observer was overwritten
  EXPECT_EQ(calls2, 1); // second observer fires
}

// ---------------------------------------------------------------------------
// checkProperties: value-change firing semantics
// ---------------------------------------------------------------------------

TEST_F(TopNFilterTest, CheckPropertiesFiresOnFirstSighting) {
  int called = 0;
  uint64_t seen = 0;
  filter_->registerObserver(kPropA, PropertyObserver{.onValueChanged = [&](uint64_t v) {
                              called++;
                              seen = v;
                            }});

  filter_->checkProperties(makeExt(kPropA, 77));
  EXPECT_EQ(called, 1);
  EXPECT_EQ(seen, 77u);
}

TEST_F(TopNFilterTest, CheckPropertiesDoesNotFireOnSameValue) {
  int called = 0;
  filter_->registerObserver(kPropA, PropertyObserver{.onValueChanged = [&](uint64_t) {
                              called++;
                            }});

  filter_->checkProperties(makeExt(kPropA, 50));
  filter_->checkProperties(makeExt(kPropA, 50)); // same value – no fire
  EXPECT_EQ(called, 1);
}

TEST_F(TopNFilterTest, CheckPropertiesFiresOnValueChange) {
  std::vector<uint64_t> values;
  filter_->registerObserver(kPropA, PropertyObserver{.onValueChanged = [&](uint64_t v) {
                              values.push_back(v);
                            }});

  filter_->checkProperties(makeExt(kPropA, 10));
  filter_->checkProperties(makeExt(kPropA, 10)); // same, no fire
  filter_->checkProperties(makeExt(kPropA, 20)); // change
  filter_->checkProperties(makeExt(kPropA, 10)); // change back

  EXPECT_THAT(values, ElementsAre(10u, 20u, 10u));
}

TEST_F(TopNFilterTest, CheckPropertiesIgnoresUnobservedType) {
  int called = 0;
  filter_->registerObserver(kPropA, PropertyObserver{.onValueChanged = [&](uint64_t) {
                              called++;
                            }});

  // Only kPropB in the extensions – should not fire kPropA observer
  filter_->checkProperties(makeExt(kPropB, 99));
  EXPECT_EQ(called, 0);
}

TEST_F(TopNFilterTest, CheckPropertiesNoObserversIsNoop) {
  // Must not crash with empty observers
  filter_->checkProperties(makeExt(kPropA, 42));
}

// ---------------------------------------------------------------------------
// Multiple property types observed independently
// ---------------------------------------------------------------------------

TEST_F(TopNFilterTest, TwoPropertyTypesObservedIndependently) {
  std::vector<uint64_t> aValues, bValues;
  filter_->registerObserver(kPropA, PropertyObserver{.onValueChanged = [&](uint64_t v) {
                              aValues.push_back(v);
                            }});
  filter_->registerObserver(kPropB, PropertyObserver{.onValueChanged = [&](uint64_t v) {
                              bValues.push_back(v);
                            }});

  Extensions ext;
  ext.insertMutableExtension(Extension{kPropA, 10});
  ext.insertMutableExtension(Extension{kPropB, 20});
  filter_->checkProperties(ext); // both fire once

  filter_->checkProperties(makeExt(kPropA, 11)); // only A changes
  filter_->checkProperties(makeExt(kPropB, 21)); // only B changes

  EXPECT_THAT(aValues, ElementsAre(10u, 11u));
  EXPECT_THAT(bValues, ElementsAre(20u, 21u));
}

// ---------------------------------------------------------------------------
// publishDone → notifyTrackEnded
// ---------------------------------------------------------------------------

TEST_F(TopNFilterTest, PublishDoneCallsOnTrackEndedOnAllObservers) {
  int endedA = 0, endedB = 0;
  filter_->registerObserver(kPropA, PropertyObserver{.onTrackEnded = [&]() { endedA++; }});
  filter_->registerObserver(kPropB, PropertyObserver{.onTrackEnded = [&]() { endedB++; }});

  EXPECT_FALSE(filter_->isEnded());

  PublishDone done;
  done.statusCode = PublishDoneStatusCode::SUBSCRIPTION_ENDED;
  filter_->publishDone(std::move(done));

  EXPECT_TRUE(filter_->isEnded());
  EXPECT_EQ(endedA, 1);
  EXPECT_EQ(endedB, 1);
}

TEST_F(TopNFilterTest, NotifyTrackEndedDirectlyFiresObservers) {
  int ended = 0;
  filter_->registerObserver(kPropA, PropertyObserver{.onTrackEnded = [&]() { ended++; }});
  filter_->notifyTrackEnded();
  EXPECT_EQ(ended, 1);
}

TEST_F(TopNFilterTest, PublishDoneDoesNotFireOnValueChanged) {
  int valueChanged = 0;
  filter_->registerObserver(kPropA, PropertyObserver{.onValueChanged = [&](uint64_t) {
                              valueChanged++;
                            }});

  PublishDone done;
  done.statusCode = PublishDoneStatusCode::SUBSCRIPTION_ENDED;
  filter_->publishDone(std::move(done));

  EXPECT_EQ(valueChanged, 0);
}

// ---------------------------------------------------------------------------
// Objects arriving after publishDone must not crash
// ---------------------------------------------------------------------------

TEST_F(TopNFilterTest, ObjectStreamAfterPublishDoneDoesNotCrash) {
  PublishDone done;
  done.statusCode = PublishDoneStatusCode::SUBSCRIPTION_ENDED;
  filter_->publishDone(std::move(done));

  // Should log a warning but not crash
  ObjectHeader hdr;
  hdr.group = 1;
  hdr.id = 0;
  hdr.extensions = makeExt(kPropA, 5);
  filter_->objectStream(hdr, nullptr, false);
}

TEST_F(TopNFilterTest, DatagramAfterPublishDoneDoesNotCrash) {
  PublishDone done;
  done.statusCode = PublishDoneStatusCode::SUBSCRIPTION_ENDED;
  filter_->publishDone(std::move(done));

  ObjectHeader hdr;
  hdr.group = 1;
  hdr.id = 0;
  hdr.extensions = makeExt(kPropA, 5);
  filter_->datagram(hdr, nullptr, false);
}

// ---------------------------------------------------------------------------
// objectStream / datagram trigger checkProperties
// ---------------------------------------------------------------------------

TEST_F(TopNFilterTest, ObjectStreamTriggersCheckProperties) {
  int called = 0;
  filter_->registerObserver(kPropA, PropertyObserver{.onValueChanged = [&](uint64_t) {
                              called++;
                            }});

  ObjectHeader hdr;
  hdr.group = 1;
  hdr.id = 0;
  hdr.extensions = makeExt(kPropA, 42);
  filter_->objectStream(hdr, nullptr, false);
  EXPECT_EQ(called, 1);
}

TEST_F(TopNFilterTest, DatagramTriggersCheckProperties) {
  int called = 0;
  filter_->registerObserver(kPropA, PropertyObserver{.onValueChanged = [&](uint64_t) {
                              called++;
                            }});

  ObjectHeader hdr;
  hdr.group = 1;
  hdr.id = 0;
  hdr.extensions = makeExt(kPropA, 99);
  filter_->datagram(hdr, nullptr, false);
  EXPECT_EQ(called, 1);
}

// ---------------------------------------------------------------------------
// Activity tracking: activityTarget_ and onActivity callback
// ---------------------------------------------------------------------------

TEST_F(TopNFilterTest, ActivityTargetOnlyUpdatedOnPropertyMatch) {
  std::chrono::steady_clock::time_point ts{};
  filter_->setActivityTarget(&ts);
  filter_->registerObserver(kPropA, PropertyObserver{});

  // No match: timestamp not written
  filter_->checkProperties(makeExt(kPropB, 99));
  EXPECT_EQ(ts, std::chrono::steady_clock::time_point{});

  // Match: timestamp written
  filter_->checkProperties(makeExt(kPropA, 1));
  auto ts1 = ts;
  EXPECT_NE(ts1, std::chrono::steady_clock::time_point{});

  // Sleep then match again: timestamp updated
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  filter_->checkProperties(makeExt(kPropA, 2));
  EXPECT_GT(ts, ts1);
}

TEST_F(TopNFilterTest, OnActivityCallbackBehavior) {
  int activityCount = 0;
  int valueChangedCount = 0;
  filter_->registerObserver(
      kPropA,
      PropertyObserver{
          .onValueChanged = [&](uint64_t) { valueChangedCount++; },
          .onActivity = [&]() { activityCount++; },
      }
  );

  // Threshold=0 (default): onActivity never fires
  filter_->checkProperties(makeExt(kPropA, 1));
  filter_->checkProperties(makeExt(kPropA, 1));
  EXPECT_EQ(activityCount, 0);

  // Enable threshold
  filter_->setActivityThreshold(std::chrono::hours(1));

  // Value changes: onValueChanged fires AND onActivity fires (two-stop throttle:
  // both can fire independently on the same object).
  filter_->checkProperties(makeExt(kPropA, 42));
  EXPECT_EQ(valueChangedCount, 2); // includes earlier call
  EXPECT_EQ(activityCount, 1);

  // Same value immediately: onActivity throttled (hours(1) not elapsed)
  filter_->checkProperties(makeExt(kPropA, 42));
  EXPECT_EQ(valueChangedCount, 2);
  EXPECT_EQ(activityCount, 1);

  // Same value again: still throttled
  filter_->checkProperties(makeExt(kPropA, 42));
  EXPECT_EQ(activityCount, 1);

  // No property match: neither callback fires
  filter_->checkProperties(makeExt(kPropB, 99));
  EXPECT_EQ(valueChangedCount, 2);
  EXPECT_EQ(activityCount, 1);
}

} // namespace
