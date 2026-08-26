/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <folly/json.h>

#include "admin/JsonStateVisitors.h"

namespace openmoq::moqx::admin {

namespace {

// Collects every chunk so a test can assert on the whole body and on how it was
// cut up.
struct Collected {
  std::vector<std::string> chunks;

  std::string body() const {
    std::string out;
    for (const auto& c : chunks) {
      out += c;
    }
    return out;
  }
};

ChunkedJsonWriter::Sink sinkInto(Collected& out) {
  return [&out](std::unique_ptr<folly::IOBuf> chunk) {
    out.chunks.push_back(chunk->moveToFbString().toStdString());
    return true;
  };
}

moxygen::FullTrackName ftn(std::vector<std::string> ns, std::string name) {
  return moxygen::FullTrackName{moxygen::TrackNamespace{std::move(ns)}, std::move(name)};
}

// One walk, exercising every section and both nesting levels of the tree.
void walk(RelayContextVisitor& v) {
  v.onRelayBegin("relay-1", 3);
  RelayStateVisitor& rv = v.onServiceBegin("default");

  rv.onPeersBegin();
  rv.onPeer("10.0.0.1:1", "peer.example", "peer-a");
  rv.onPeer("10.0.0.2:2", "other.example", "");
  rv.onPeersEnd();

  rv.onSubscriptionsBegin();
  auto first = ftn({"ns", "sub"}, "track");
  RelayStateVisitor::SubscriptionInfo a{
      .ftn = first,
      .isPublish = true,
      .largest = moxygen::AbsoluteLocation{7, 2},
      .totalGroupsReceived = 4,
      .totalObjectsReceived = 9,
      .sourceAddress = "1.2.3.4:5",
  };
  rv.onSubscription(a);
  auto second = ftn({"ns"}, "quiet");
  RelayStateVisitor::SubscriptionInfo b{.ftn = second, .isPublish = false};
  rv.onSubscription(b);
  rv.onSubscriptionsEnd();

  rv.onNamespaceTreeBegin();
  rv.beginNamespaceNode("", moxygen::TrackNamespace{}, 0, "", "");
  rv.beginNamespaceNode("test", moxygen::TrackNamespace{{"test"}}, 2, "10.0.0.1:1", "peer-a");
  rv.beginNamespaceNode("inner", moxygen::TrackNamespace{{"test", "inner"}}, 1, "", "");
  rv.endNamespaceNode();
  rv.endNamespaceNode();
  rv.endNamespaceNode();
  rv.onNamespaceTreeEnd();

  auto now = MoqxCache::TimePoint{} + std::chrono::seconds(100);
  auto writtenName = ftn({"ns"}, "t");
  auto unwrittenName = ftn({"ns"}, "u");
  std::vector<MoqxCache::GroupStats> groups{{3, 8}};
  std::vector<MoqxCache::GroupStats> noGroups;
  rv.onCacheBegin(42, now);
  rv.onCacheTrack({writtenName, false, now - std::chrono::milliseconds(250), groups});
  rv.onCacheTrack({unwrittenName, true, MoqxCache::TimePoint::min(), noGroups});
  rv.onCacheEnd();

  v.onServiceUpstream("moqt://up/rel", "connected");
  v.onServiceEnd();
  v.onRelayEnd();
}

std::string bodyAtThreshold(size_t threshold, Collected& out) {
  ChunkedJsonWriter w(sinkInto(out), threshold);
  JsonRelayContextVisitor visitor(w);
  walk(visitor);
  w.flush();
  return out.body();
}

std::string bodyAtThreshold(size_t threshold) {
  Collected out;
  return bodyAtThreshold(threshold, out);
}

} // namespace

// The load-bearing property: comma and nesting state live in the JsonWriter,
// and the QueueAppender is reset after each move, so a flush can land anywhere
// without disturbing the bytes.
// Round-trips every escape the writer knows about, at thresholds that cut the
// string apart mid-escape.
TEST(StateStreamTest, StringsSurviveEscapingAndChunking) {
  const std::string tricky =
      "a long unescaped run \"quoted\" back\\slash new\nline tab\there \x01 ctrl \x1f end";
  for (size_t threshold : {size_t(1), size_t(3), size_t(16), size_t(4096)}) {
    Collected out;
    ChunkedJsonWriter w(sinkInto(out), threshold);
    w.json().beginObject();
    w.json().key("s");
    w.json().strVal(tricky);
    w.json().endObject();
    w.flush();

    auto parsed = folly::parseJson(out.body());
    EXPECT_EQ(parsed["s"].asString(), tricky) << "threshold " << threshold;
  }
}

TEST(StateStreamTest, ChunkBoundariesAreInvisible) {
  const auto reference = bodyAtThreshold(ChunkedJsonWriter::kDefaultThreshold);

  for (size_t threshold : {size_t(1), size_t(7), size_t(64), size_t(512), size_t(4096)}) {
    Collected out;
    EXPECT_EQ(bodyAtThreshold(threshold, out), reference) << "threshold=" << threshold;
  }
}

TEST(StateStreamTest, SmallThresholdActuallySplits) {
  Collected out;
  bodyAtThreshold(1, out);
  EXPECT_GT(out.chunks.size(), 1u);

  Collected single;
  bodyAtThreshold(ChunkedJsonWriter::kDefaultThreshold, single);
  EXPECT_EQ(single.chunks.size(), 1u) << "a small /state should stay one chunk";
}

TEST(StateStreamTest, BodyIsValidJsonWithTrailingNewline) {
  auto body = bodyAtThreshold(1);
  ASSERT_FALSE(body.empty());
  EXPECT_EQ(body.back(), '\n');

  auto parsed = folly::parseJson(body);
  EXPECT_EQ(parsed["relay_id"].asString(), "relay-1");
  EXPECT_EQ(parsed["active_sessions"].asInt(), 3);
  ASSERT_TRUE(parsed["services"].count("default"));
}

TEST(StateStreamTest, PeersCarryAddressAndOmitEmptyRelayId) {
  auto parsed = folly::parseJson(bodyAtThreshold(1))["services"]["default"];
  const auto& peers = parsed["downstream_peers"];
  ASSERT_EQ(peers.size(), 2);
  EXPECT_EQ(peers[0]["address"].asString(), "10.0.0.1:1");
  EXPECT_EQ(peers[0]["relay_id"].asString(), "peer-a");
  EXPECT_EQ(peers[1].count("relay_id"), 0) << "an empty relay_id is omitted, not emitted as \"\"";
}

TEST(StateStreamTest, SubscriptionCarriesIngestCountersAndOmitsSubscriberCounts) {
  auto body = bodyAtThreshold(1);
  auto sub = folly::parseJson(body)["services"]["default"]["subscriptions"][0];

  EXPECT_EQ(folly::toJson(sub["namespace"]), R"(["ns","sub"])");
  EXPECT_EQ(sub["track_name"].asString(), "track");
  EXPECT_TRUE(sub["is_publish"].asBool());
  EXPECT_EQ(sub["total_groups_received"].asInt(), 4);
  EXPECT_EQ(sub["total_objects_received"].asInt(), 9);
  EXPECT_EQ(sub["largest"]["group"].asInt(), 7);
  EXPECT_EQ(sub["largest"]["object"].asInt(), 2);

  // Dropped outright: in LocalForwarder mode these counted io threads, not
  // subscribers. /metrics/track carries the real numbers.
  EXPECT_EQ(body.find("forwarding_subscribers"), std::string::npos) << body;
  EXPECT_EQ(body.find(R"("subscribers")"), std::string::npos) << body;
}

TEST(StateStreamTest, OmitsLargestWhenUnset) {
  auto sub = folly::parseJson(bodyAtThreshold(1))["services"]["default"]["subscriptions"][1];
  EXPECT_EQ(sub.count("largest"), 0);
  EXPECT_EQ(sub["total_objects_received"].asInt(), 0);
}

TEST(StateStreamTest, NestsNamespaceChildrenByKey) {
  auto tree = folly::parseJson(bodyAtThreshold(1))["services"]["default"]["namespace_tree"];
  const auto& test = tree["children"]["test"];
  EXPECT_EQ(folly::toJson(test["full_namespace"]), R"(["test"])");
  EXPECT_EQ(test["namespace_subscribers"].asInt(), 2);
  EXPECT_EQ(test["publisher"].asString(), "10.0.0.1:1");
  EXPECT_EQ(test["peer_id"].asString(), "peer-a");

  const auto& inner = test["children"]["inner"];
  EXPECT_EQ(folly::toJson(inner["full_namespace"]), R"(["test","inner"])");
  // A node with no publisher or peer omits both rather than emitting "".
  EXPECT_EQ(inner.count("publisher"), 0);
  EXPECT_EQ(inner.count("peer_id"), 0);
}

TEST(StateStreamTest, CacheAgeIsRelativeToTheCapturedInstant) {
  auto cache = folly::parseJson(bodyAtThreshold(1))["services"]["default"]["cache"];
  EXPECT_EQ(cache["total_bytes"].asInt(), 42);
  EXPECT_EQ(cache["tracks"][0]["last_write_ms_ago"].asInt(), 250);
  // Field-wise, not toJson: dynamic objects are hash maps, so their key order
  // is not stable enough to compare serialized.
  const auto& groups = cache["tracks"][0]["groups"];
  ASSERT_EQ(groups.size(), 1);
  EXPECT_EQ(groups[0]["group_id"].asInt(), 3);
  EXPECT_EQ(groups[0]["objects"].asInt(), 8);
  // An unwritten track reports null rather than an absurd age.
  EXPECT_TRUE(cache["tracks"][1]["last_write_ms_ago"].isNull());
}

// onServiceUpstream runs after the relay walk, so upstream trails cache. Any
// consumer diffing raw /state sees that order, so it is pinned positionally --
// parsing cannot express key order.
TEST(StateStreamTest, UpstreamFollowsCache) {
  auto body = bodyAtThreshold(1);
  auto cachePos = body.find(R"("cache")");
  auto upstreamPos = body.find(R"("upstream")");
  ASSERT_NE(cachePos, std::string::npos) << body;
  ASSERT_NE(upstreamPos, std::string::npos) << body;
  EXPECT_LT(cachePos, upstreamPos) << body;

  auto upstream = folly::parseJson(body)["services"]["default"]["upstream"];
  EXPECT_EQ(upstream["url"].asString(), "moqt://up/rel");
  EXPECT_EQ(upstream["state"].asString(), "connected");
}

// A sink that has given up stops the walk rather than letting it run to the end
// writing bytes nobody will read.
TEST(StateStreamTest, DeadSinkStopsReportingAlive) {
  ChunkedJsonWriter w([](std::unique_ptr<folly::IOBuf>) { return false; }, /*threshold=*/1);
  JsonRelayContextVisitor visitor(w);
  EXPECT_TRUE(visitor.alive());
  walk(visitor);
  EXPECT_FALSE(visitor.alive());
}

} // namespace openmoq::moqx::admin
