/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MoqxRelayTestFixture.h"

#include <atomic>
#include <thread>

#include "relay/TrackStatsFilter.h"

namespace openmoq::moqx::test {

namespace {

Payload makePayload(size_t bytes) {
  return folly::IOBuf::copyBuffer(std::string(bytes, 'x'));
}

} // namespace

class MoQRelayTrackStatsTest : public MoQRelayTest {
protected:
  // Only this fixture counts, so the registry is bound here rather than for
  // every relay test. bindAll blocks until each executor's thread runs the
  // bind, and this thread is what drives exec_, so pump it from here while a
  // helper waits.
  void SetUp() override {
    MoQRelayTest::SetUp();
    std::vector<folly::Executor*> execs{static_cast<moxygen::MoQExecutor*>(exec_.get())};
    if (relayEvb_) {
      execs.push_back(relay_->getRelayExec());
    }
    std::atomic<bool> done{false};
    std::thread binder([&] {
      relay_->trackStatsRegistry().bindAll(execs);
      done = true;
      // Wakes the pump if it is already parked in drive() on an empty queue.
      exec_->add([] {});
    });
    while (!done) {
      exec_->drive();
    }
    binder.join();
  }

  // Which thread counts what varies by mode — ingest is on the relay exec in
  // MT/LF, egress on the relay exec in MT but the subscriber thread in LF — so
  // merge every thread's counters, exactly as aggregateTrackStats does.
  stats::TrackCounters trackCounters(const FullTrackName& ftn) {
    auto merged = countersOnThisThread(ftn);
    if (relayEvb_) {
      stats::TrackCounters relayCounters;
      verifyOnRelayExec([&] { relayCounters = countersOnThisThread(ftn); });
      merged += relayCounters;
    }
    return merged;
  }

private:
  stats::TrackCounters countersOnThisThread(const FullTrackName& ftn) {
    auto* collector = relay_->trackStatsRegistry().currentCollector();
    EXPECT_NE(collector, nullptr);
    auto stats = collector->get(ftn);
    return stats ? stats->counters() : stats::TrackCounters{};
  }
};

TEST_P(MoQRelayTrackStatsTest, CountsIngestObjectsAndBytes) {
  auto publisherSession = createMockSession();
  auto subscriber = createMockSession();
  auto mockConsumer = createMockConsumer();
  auto mockSg = createMockSubgroupConsumer();

  EXPECT_CALL(*mockConsumer, beginSubgroup(0, 0, _, _))
      .WillRepeatedly([&](uint64_t, uint64_t, uint8_t, moxygen::BeginSubgroupOptions) {
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(mockSg);
      });
  EXPECT_CALL(*mockSg, object(_, _, _, _))
      .WillRepeatedly(Return(folly::makeExpected<MoQPublishError>(folly::unit)));

  auto publishConsumer = doPublish(publisherSession, kTestTrackName);
  subscribeToTrack(subscriber, kTestTrackName, mockConsumer, RequestID(1));

  auto sg = publishConsumer->beginSubgroup(0, 0, 0);
  ASSERT_TRUE(sg.hasValue());
  EXPECT_TRUE(sg.value()->object(0, makePayload(10)).hasValue());
  EXPECT_TRUE(sg.value()->object(1, makePayload(20)).hasValue());
  EXPECT_TRUE(sg.value()->endOfSubgroup().hasValue());
  driveIfMultiThread();

  auto ingest = trackCounters(kTestTrackName);
  EXPECT_EQ(ingest.received.groups, 1);
  EXPECT_EQ(ingest.received.subgroups, 1);
  EXPECT_EQ(ingest.received.objects, 2);
  EXPECT_EQ(ingest.received.bytes, 30);
  EXPECT_NE(ingest.publishStart, stats::TrackClock::time_point{});
  EXPECT_NE(ingest.lastObject, stats::TrackClock::time_point{});

  removeSession(publisherSession);
  removeSession(subscriber);
  exec_->drive();
}

TEST_P(MoQRelayTrackStatsTest, CountsGroupsByTransition) {
  auto publisherSession = createMockSession();
  auto subscriber = createMockSession();
  auto mockConsumer = createMockConsumer();
  auto mockSg = createMockSubgroupConsumer();

  EXPECT_CALL(*mockConsumer, beginSubgroup(_, _, _, _))
      .WillRepeatedly([&](uint64_t, uint64_t, uint8_t, moxygen::BeginSubgroupOptions) {
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(mockSg);
      });
  EXPECT_CALL(*mockSg, object(_, _, _, _))
      .WillRepeatedly(Return(folly::makeExpected<MoQPublishError>(folly::unit)));

  auto publishConsumer = doPublish(publisherSession, kTestTrackName);
  subscribeToTrack(subscriber, kTestTrackName, mockConsumer, RequestID(1));

  for (uint64_t group = 0; group < 3; ++group) {
    auto sg = publishConsumer->beginSubgroup(group, 0, 0);
    ASSERT_TRUE(sg.hasValue()) << "group " << group << ": " << sg.error().describe();
    EXPECT_TRUE(sg.value()->object(0, makePayload(5)).hasValue());
    EXPECT_TRUE(sg.value()->endOfSubgroup().hasValue());
    driveIfMultiThread();
  }

  auto ingest = trackCounters(kTestTrackName);
  EXPECT_EQ(ingest.received.groups, 3);
  EXPECT_EQ(ingest.received.subgroups, 3);
  EXPECT_EQ(ingest.received.objects, 3);
  EXPECT_EQ(ingest.received.bytes, 15);

  removeSession(publisherSession);
  removeSession(subscriber);
  exec_->drive();
}

TEST_P(MoQRelayTrackStatsTest, CountsEgressPerSubscriber) {
  auto publisherSession = createMockSession();
  auto sub1 = createMockSession();
  auto sub2 = createMockSession();
  auto mockConsumer1 = createMockConsumer();
  auto mockConsumer2 = createMockConsumer();
  auto mockSg1 = createMockSubgroupConsumer();
  auto mockSg2 = createMockSubgroupConsumer();

  EXPECT_CALL(*mockConsumer1, beginSubgroup(0, 0, _, _))
      .WillRepeatedly([&](uint64_t, uint64_t, uint8_t, moxygen::BeginSubgroupOptions) {
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(mockSg1);
      });
  EXPECT_CALL(*mockConsumer2, beginSubgroup(0, 0, _, _))
      .WillRepeatedly([&](uint64_t, uint64_t, uint8_t, moxygen::BeginSubgroupOptions) {
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(mockSg2);
      });
  EXPECT_CALL(*mockSg1, object(_, _, _, _))
      .WillRepeatedly(Return(folly::makeExpected<MoQPublishError>(folly::unit)));
  EXPECT_CALL(*mockSg2, object(_, _, _, _))
      .WillRepeatedly(Return(folly::makeExpected<MoQPublishError>(folly::unit)));

  auto publishConsumer = doPublish(publisherSession, kTestTrackName);
  subscribeToTrack(sub1, kTestTrackName, mockConsumer1, RequestID(1));
  subscribeToTrack(sub2, kTestTrackName, mockConsumer2, RequestID(2));
  driveIfMultiThread();

  EXPECT_EQ(trackCounters(kTestTrackName).subscribers, 2);

  auto sg = publishConsumer->beginSubgroup(0, 0, 0);
  ASSERT_TRUE(sg.hasValue());
  EXPECT_TRUE(sg.value()->object(0, makePayload(10)).hasValue());
  EXPECT_TRUE(sg.value()->endOfSubgroup().hasValue());
  driveIfMultiThread();

  // One object fans out to both subscribers.
  auto egress = trackCounters(kTestTrackName);
  EXPECT_EQ(egress.sent.objects, 2);
  EXPECT_EQ(egress.sent.bytes, 20);
  EXPECT_EQ(egress.sent.subgroups, 2);
  EXPECT_EQ(egress.sent.groups, 2);

  removeSession(publisherSession);
  removeSession(sub1);
  removeSession(sub2);
  exec_->drive();
}

TEST_P(MoQRelayTrackStatsTest, SubscriberGaugeDropsOnUnsubscribe) {
  auto publisherSession = createMockSession();
  auto subscriber = createMockSession();
  auto mockConsumer = createMockConsumer();

  auto publishConsumer = doPublish(publisherSession, kTestTrackName);
  subscribeToTrack(subscriber, kTestTrackName, mockConsumer, RequestID(1));
  driveIfMultiThread();
  EXPECT_EQ(trackCounters(kTestTrackName).subscribers, 1);

  removeSession(subscriber);
  exec_->drive();
  EXPECT_EQ(trackCounters(kTestTrackName).subscribers, 0);

  removeSession(publisherSession);
  exec_->drive();
}

TEST_P(MoQRelayTrackStatsTest, MatchTracksFiltersAndReportsTruncation) {
  auto publisherSession = createMockSession();
  const FullTrackName other{kTestNamespace, "track2"};
  auto publishConsumer1 = doPublish(publisherSession, kTestTrackName);
  auto publishConsumer2 = doPublish(publisherSession, other);
  driveIfMultiThread();

  verifyOnRelayExec([&] {
    auto all = relay_->matchTracks(kTestNamespace, nullptr, 10);
    EXPECT_EQ(all.matched, 2);
    EXPECT_EQ(all.keys.size(), 2);

    auto exact = relay_->matchTracks(kTestNamespace, &kTestTrackName.trackName, 10);
    EXPECT_EQ(exact.matched, 1);
    ASSERT_EQ(exact.keys.size(), 1);
    EXPECT_EQ(exact.keys[0], kTestTrackName);

    auto limited = relay_->matchTracks(kTestNamespace, nullptr, 1);
    EXPECT_EQ(limited.matched, 2);
    EXPECT_EQ(limited.keys.size(), 1);

    const TrackNamespace unrelated{{"test", "other"}};
    EXPECT_EQ(relay_->matchTracks(unrelated, nullptr, 10).matched, 0);
  });

  removeSession(publisherSession);
  exec_->drive();
}

// The relay also fans out via PUBLISH (SUBSCRIBE_NAMESPACE with the publish
// option, TRACK_FILTER selection), which reaches subscribers through
// startPublish rather than the subscribe path.
TEST_P(MoQRelayTrackStatsTest, CountsEgressOnPublishFanout) {
  auto publisherSession = createMockSession();
  auto subscriber = createMockSession();
  auto mockConsumer = createMockConsumer();
  auto mockSg = createMockSubgroupConsumer();
  std::atomic<bool> published{false};

  EXPECT_CALL(*subscriber, publish(testing::_, testing::_))
      .WillOnce([&](const PublishRequest&, auto /*subHandle*/) {
        published.store(true);
        return Subscriber::PublishResult(Subscriber::PublishConsumerAndReplyTask{
            mockConsumer,
            []() -> folly::coro::Task<folly::Expected<PublishOk, PublishError>> {
              co_return PublishOk{
                  RequestID(1),
                  true,
                  0,
                  GroupOrder::OldestFirst,
                  LocationType::LargestObject,
                  std::nullopt,
                  std::nullopt
              };
            }()
        });
      });
  EXPECT_CALL(*mockConsumer, beginSubgroup(0, 0, _, _))
      .WillRepeatedly([&](uint64_t, uint64_t, uint8_t, moxygen::BeginSubgroupOptions) {
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(mockSg);
      });
  EXPECT_CALL(*mockSg, object(_, _, _, _))
      .WillRepeatedly(Return(folly::makeExpected<MoQPublishError>(folly::unit)));

  doSubscribeNamespace(subscriber, kTestNamespace);
  auto publishConsumer = doPublish(publisherSession, kTestTrackName);
  ASSERT_TRUE(driveUntil([&] { return published.load(); }))
      << "publish was not forwarded to the subscriber";

  EXPECT_EQ(trackCounters(kTestTrackName).subscribers, 1);

  auto sg = publishConsumer->beginSubgroup(0, 0, 0);
  ASSERT_TRUE(sg.hasValue()) << sg.error().describe();
  EXPECT_TRUE(sg.value()->object(0, makePayload(7)).hasValue());
  EXPECT_TRUE(sg.value()->endOfSubgroup().hasValue());
  driveIfMultiThread();

  auto counters = trackCounters(kTestTrackName);
  EXPECT_EQ(counters.received.objects, 1);
  EXPECT_EQ(counters.sent.objects, 1);
  EXPECT_EQ(counters.sent.bytes, 7);

  removeSession(publisherSession);
  removeSession(subscriber);
  exec_->drive();
}

// Subgroups of concurrently-open groups interleave; the window keeps both.
TEST_P(MoQRelayTrackStatsTest, CountsInterleavedGroupsOnce) {
  auto publisherSession = createMockSession();
  auto subscriber = createMockSession();
  auto mockConsumer = createMockConsumer();
  auto mockSg = createMockSubgroupConsumer();

  EXPECT_CALL(*mockConsumer, beginSubgroup(_, _, _, _))
      .WillRepeatedly([&](uint64_t, uint64_t, uint8_t, moxygen::BeginSubgroupOptions) {
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(mockSg);
      });
  EXPECT_CALL(*mockSg, object(_, _, _, _))
      .WillRepeatedly(Return(folly::makeExpected<MoQPublishError>(folly::unit)));

  auto publishConsumer = doPublish(publisherSession, kTestTrackName);
  subscribeToTrack(subscriber, kTestTrackName, mockConsumer, RequestID(1));

  // G0/S0, G1/S0, G0/S1, G1/S1.
  for (uint64_t subgroup = 0; subgroup < 2; ++subgroup) {
    for (uint64_t group = 0; group < 2; ++group) {
      auto sg = publishConsumer->beginSubgroup(group, subgroup, 0);
      ASSERT_TRUE(sg.hasValue()) << sg.error().describe();
      EXPECT_TRUE(sg.value()->object(0, makePayload(1)).hasValue());
      EXPECT_TRUE(sg.value()->endOfSubgroup().hasValue());
      driveIfMultiThread();
    }
  }

  auto counters = trackCounters(kTestTrackName);
  EXPECT_EQ(counters.received.groups, 2);
  EXPECT_EQ(counters.received.subgroups, 4);

  removeSession(publisherSession);
  removeSession(subscriber);
  exec_->drive();
}

// LRU, not insertion order: a group still receiving subgroups survives more
// than kRecentGroups other groups opening alongside it.
TEST_P(MoQRelayTrackStatsTest, ActiveGroupSurvivesWindowChurn) {
  auto publisherSession = createMockSession();
  auto subscriber = createMockSession();
  auto mockConsumer = createMockConsumer();
  auto mockSg = createMockSubgroupConsumer();

  EXPECT_CALL(*mockConsumer, beginSubgroup(_, _, _, _))
      .WillRepeatedly([&](uint64_t, uint64_t, uint8_t, moxygen::BeginSubgroupOptions) {
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(mockSg);
      });

  auto publishConsumer = doPublish(publisherSession, kTestTrackName);
  subscribeToTrack(subscriber, kTestTrackName, mockConsumer, RequestID(1));

  const uint64_t kLongLived = 0;
  const size_t window = TrackStatsFilter::kRecentGroups;
  auto openSubgroup = [&](uint64_t group, uint64_t subgroup) {
    auto sg = publishConsumer->beginSubgroup(group, subgroup, 0);
    ASSERT_TRUE(sg.hasValue()) << sg.error().describe();
    EXPECT_TRUE(sg.value()->endOfSubgroup().hasValue());
    driveIfMultiThread();
  };

  openSubgroup(kLongLived, 0);
  // Each new group is followed by another subgroup of the long-lived group,
  // refreshing it; under insertion-order eviction it would have aged out.
  for (uint64_t group = 1; group <= window; ++group) {
    openSubgroup(group, 0);
    openSubgroup(kLongLived, group);
  }

  EXPECT_EQ(trackCounters(kTestTrackName).received.groups, window + 1);

  removeSession(publisherSession);
  removeSession(subscriber);
  exec_->drive();
}

// Past the window a revisited group counts again — the bound is deliberate.
TEST_P(MoQRelayTrackStatsTest, RecountsGroupEvictedFromWindow) {
  auto publisherSession = createMockSession();
  auto subscriber = createMockSession();
  auto mockConsumer = createMockConsumer();
  auto mockSg = createMockSubgroupConsumer();

  EXPECT_CALL(*mockConsumer, beginSubgroup(_, _, _, _))
      .WillRepeatedly([&](uint64_t, uint64_t, uint8_t, moxygen::BeginSubgroupOptions) {
        return folly::makeExpected<MoQPublishError, std::shared_ptr<SubgroupConsumer>>(mockSg);
      });

  auto publishConsumer = doPublish(publisherSession, kTestTrackName);
  subscribeToTrack(subscriber, kTestTrackName, mockConsumer, RequestID(1));

  const size_t window = TrackStatsFilter::kRecentGroups;
  for (uint64_t group = 0; group <= window; ++group) {
    auto sg = publishConsumer->beginSubgroup(group, 0, 0);
    ASSERT_TRUE(sg.hasValue()) << sg.error().describe();
    EXPECT_TRUE(sg.value()->endOfSubgroup().hasValue());
    driveIfMultiThread();
  }
  auto sg = publishConsumer->beginSubgroup(0, 1, 0);
  ASSERT_TRUE(sg.hasValue()) << sg.error().describe();
  EXPECT_TRUE(sg.value()->endOfSubgroup().hasValue());
  driveIfMultiThread();

  EXPECT_EQ(trackCounters(kTestTrackName).received.groups, window + 2);

  removeSession(publisherSession);
  removeSession(subscriber);
  exec_->drive();
}

INSTANTIATE_TEST_SUITE_P(
    AllModes,
    MoQRelayTrackStatsTest,
    ::testing::Values(RelayMode::SingleThread, RelayMode::MultiThread, RelayMode::LocalForwarderMT),
    [](const ::testing::TestParamInfo<RelayMode>& info) {
      std::ostringstream os;
      PrintTo(info.param, &os);
      return os.str();
    }
);

} // namespace openmoq::moqx::test
