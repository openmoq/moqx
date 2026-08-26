/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MoqxRelayTestFixture.h"

#include <folly/json.h>

#include "admin/JsonStateVisitors.h"

namespace openmoq::moqx::test {

namespace {

// Mirrors what the /state handler does for one service, minus the HTTP layer:
// the walk runs on the executor that owns relay state and writes JSON as it
// goes. Threshold of 1 so every item is flushed separately -- if a chunk
// boundary could corrupt the output, these tests would see it too.
class StateWalk {
public:
  explicit StateWalk(size_t threshold = 1)
      : writer_(
            [this](std::unique_ptr<folly::IOBuf> chunk) {
              body_ += chunk->moveToFbString().toStdString();
              return true;
            },
            threshold
        ),
        visitor_(writer_) {
    // dumpState emits a relay fragment, not a document; brace it so the result
    // can be parsed.
    writer_.json().beginObject();
  }

  admin::JsonRelayStateVisitor& visitor() { return visitor_; }

  folly::dynamic finish() {
    writer_.json().endObject();
    writer_.flush();
    return folly::parseJson(body_);
  }

private:
  std::string body_;
  admin::ChunkedJsonWriter writer_;
  admin::JsonRelayStateVisitor visitor_;
};

const folly::dynamic* findSubscription(const folly::dynamic& state, const std::string& trackName) {
  for (const auto& sub : state["subscriptions"]) {
    if (sub["track_name"].asString() == trackName) {
      return &sub;
    }
  }
  return nullptr;
}

} // namespace

class MoQRelayStateTest : public MoQRelayTest {
protected:
  // dumpState is synchronous and must run where relay state lives, which is
  // exactly what verifyOnRelayExec provides in every mode.
  folly::dynamic dumpState() {
    StateWalk walk;
    verifyOnRelayExec([&] { relay_->dumpState(walk.visitor()); });
    return walk.finish();
  }
};

TEST_P(MoQRelayStateTest, EmptyRelayHasRootTreeAndNoSubscriptions) {
  auto state = dumpState();

  EXPECT_EQ(state["subscriptions"].size(), 0);
  // The walk always emits a root node, even with nothing published.
  ASSERT_TRUE(state.count("namespace_tree"));
  EXPECT_EQ(state["namespace_tree"]["children"].size(), 0);
  // maxCachedTracks = 0 in the fixture, so the cache section is absent.
  EXPECT_EQ(state.count("cache"), 0);
}

TEST_P(MoQRelayStateTest, PublishedTracksAppearInEveryMode) {
  auto publisherSession = createMockSession();
  const FullTrackName second{kTestNamespace, "track2"};
  doPublish(publisherSession, kTestTrackName);
  doPublish(publisherSession, second);
  driveIfMultiThread();

  auto state = dumpState();

  EXPECT_EQ(state["subscriptions"].size(), 2);
  const auto* first = findSubscription(state, "track1");
  ASSERT_NE(first, nullptr) << folly::toJson(state);
  EXPECT_TRUE((*first)["is_publish"].asBool());
  EXPECT_NE(findSubscription(state, "track2"), nullptr) << folly::toJson(state);

  removeSession(publisherSession);
  exec_->drive();
}

// The bug this whole change exists for: in LocalForwarder mode the registry's
// forwarder is owned by another executor, so reading these counters off it
// reported zeros. They are now counted by RelayIngestFilter on the relay
// executor, which sees every object in every mode.
TEST_P(MoQRelayStateTest, IngestCountersAreReportedInEveryMode) {
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
  ASSERT_TRUE(sg.hasValue()) << sg.error().describe();
  EXPECT_TRUE(sg.value()->object(0, folly::IOBuf::copyBuffer("abc")).hasValue());
  EXPECT_TRUE(sg.value()->object(1, folly::IOBuf::copyBuffer("de")).hasValue());
  EXPECT_TRUE(sg.value()->endOfSubgroup().hasValue());
  driveIfMultiThread();

  auto state = dumpState();

  const auto* sub = findSubscription(state, "track1");
  ASSERT_NE(sub, nullptr) << folly::toJson(state);
  EXPECT_EQ((*sub)["total_groups_received"].asInt(), 1);
  EXPECT_EQ((*sub)["total_objects_received"].asInt(), 2);
  ASSERT_TRUE(sub->count("largest")) << folly::toJson(*sub);
  EXPECT_EQ((*sub)["largest"]["group"].asInt(), 0);
  EXPECT_EQ((*sub)["largest"]["object"].asInt(), 1);

  removeSession(publisherSession);
  removeSession(subscriber);
  exec_->drive();
}

// Subgroups of different groups interleave on the wire, so a one-deep "did the
// group change" check counts every switch as a new group. The MRU window
// shared with TrackStatsFilter is what keeps this at 2.
TEST_P(MoQRelayStateTest, InterleavedGroupsAreCountedOnce) {
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

  auto first = publishConsumer->beginSubgroup(0, 0, 0);
  ASSERT_TRUE(first.hasValue()) << first.error().describe();
  auto second = publishConsumer->beginSubgroup(1, 0, 0);
  ASSERT_TRUE(second.hasValue()) << second.error().describe();
  EXPECT_TRUE(first.value()->object(0, folly::IOBuf::copyBuffer("a")).hasValue());
  EXPECT_TRUE(second.value()->object(0, folly::IOBuf::copyBuffer("b")).hasValue());
  EXPECT_TRUE(first.value()->object(1, folly::IOBuf::copyBuffer("c")).hasValue());
  EXPECT_TRUE(second.value()->object(1, folly::IOBuf::copyBuffer("d")).hasValue());
  EXPECT_TRUE(first.value()->endOfSubgroup().hasValue());
  EXPECT_TRUE(second.value()->endOfSubgroup().hasValue());
  driveIfMultiThread();

  auto state = dumpState();

  const auto* sub = findSubscription(state, "track1");
  ASSERT_NE(sub, nullptr) << folly::toJson(state);
  EXPECT_EQ((*sub)["total_groups_received"].asInt(), 2) << "two groups, four switches";
  EXPECT_EQ((*sub)["total_objects_received"].asInt(), 4);
  EXPECT_EQ((*sub)["largest"]["group"].asInt(), 1);
  EXPECT_EQ((*sub)["largest"]["object"].asInt(), 1);

  removeSession(publisherSession);
  removeSession(subscriber);
  exec_->drive();
}

TEST_P(MoQRelayStateTest, AnnouncedNamespaceAppearsInTree) {
  auto publisherSession = createMockSession();
  doPublishNamespace(publisherSession, kTestNamespace);
  driveIfMultiThread();

  auto state = dumpState();

  // kTestNamespace is {"test","namespace"} and kAllowedPrefix is {"test"}, so
  // the tree nests one level per tuple below the root.
  const auto& root = state["namespace_tree"];
  ASSERT_EQ(root["children"].size(), 1) << folly::toJson(state);
  const auto& test = root["children"]["test"];
  ASSERT_EQ(test["children"].size(), 1) << folly::toJson(test);
  const auto& inner = test["children"]["namespace"];
  EXPECT_EQ(folly::toJson(inner["full_namespace"]), R"(["test","namespace"])");

  removeSession(publisherSession);
  exec_->drive();
}

INSTANTIATE_TEST_SUITE_P(
    AllModes,
    MoQRelayStateTest,
    ::testing::Values(RelayMode::SingleThread, RelayMode::MultiThread, RelayMode::LocalForwarderMT),
    [](const ::testing::TestParamInfo<RelayMode>& info) {
      std::ostringstream os;
      PrintTo(info.param, &os);
      return os.str();
    }
);

} // namespace openmoq::moqx::test
