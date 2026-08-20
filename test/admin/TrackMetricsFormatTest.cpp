/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <regex>

#include "admin/TrackMetricsHandler.h"

namespace openmoq::moqx::admin {

namespace {

MoqxRelayContext::TrackMetricsResult
makeResult(stats::TrackCounters counters, std::string service) {
  MoqxRelayContext::TrackMetricsResult result;
  result.matched = 1;
  result.tracks.push_back(
      {std::move(service),
       moxygen::FullTrackName{moxygen::TrackNamespace("ns", "/"), "track"},
       counters}
  );
  return result;
}

std::string format(const MoqxRelayContext::TrackMetricsResult& result, bool omitMetadata = false) {
  auto body = formatTrackMetrics(result, omitMetadata);
  return body->moveToFbString().toStdString();
}

} // namespace

TEST(TrackMetricsFormatTest, EmitsCountersFromBothDirections) {
  stats::TrackCounters counters;
  counters.received.objects = 7;
  counters.received.bytes = 100;
  counters.sent.objects = 21;
  counters.subscribers = 3;

  auto out = format(makeResult(counters, "svc"));

  EXPECT_NE(
      out.find(
          "moqx_track_objects_received_total{service=\"svc\",namespace=\"ns\",track=\"track\"} 7\n"
      ),
      std::string::npos
  );
  EXPECT_NE(
      out.find(
          "moqx_track_bytes_received_total{service=\"svc\",namespace=\"ns\",track=\"track\"} 100\n"
      ),
      std::string::npos
  );
  EXPECT_NE(
      out.find(
          "moqx_track_objects_sent_total{service=\"svc\",namespace=\"ns\",track=\"track\"} 21\n"
      ),
      std::string::npos
  );
  EXPECT_NE(
      out.find("moqx_track_subscribers{service=\"svc\",namespace=\"ns\",track=\"track\"} 3\n"),
      std::string::npos
  );
}

// Prometheus expresses time in seconds, so millisecond resolution has to be a
// fraction rather than a different unit or a renamed metric.
TEST(TrackMetricsFormatTest, TimestampsCarryMillisecondFraction) {
  stats::TrackCounters counters;
  counters.publishStart = stats::TrackClock::now();
  counters.lastObject = counters.publishStart;

  auto out = format(makeResult(counters, "svc"));

  std::regex lastObject{R"(moqx_track_last_object_timestamp_seconds\{[^}]*\} (\d{10,})\.(\d{3})\n)"
  };
  std::smatch match;
  ASSERT_TRUE(std::regex_search(out, match, lastObject)) << out;
  EXPECT_EQ(match[2].str().size(), 3u);

  std::regex publishStart{R"(moqx_track_publish_start_timestamp_seconds\{[^}]*\} \d{10,}\.\d{3}\n)"
  };
  EXPECT_TRUE(std::regex_search(out, publishStart)) << out;
}

// A track that has not seen an object has no meaningful timestamp, and 0 would
// render as 1970 in any time()-based panel.
TEST(TrackMetricsFormatTest, OmitsUnsetTimestamps) {
  stats::TrackCounters counters;
  counters.received.objects = 1;

  auto out = format(makeResult(counters, "svc"));

  EXPECT_NE(out.find("# TYPE moqx_track_last_object_timestamp_seconds gauge"), std::string::npos);
  EXPECT_EQ(out.find("moqx_track_last_object_timestamp_seconds{"), std::string::npos);
  EXPECT_EQ(out.find("moqx_track_publish_start_timestamp_seconds{"), std::string::npos);
}

TEST(TrackMetricsFormatTest, OmitMetadataDropsHelpAndTypeButKeepsSamples) {
  stats::TrackCounters counters;
  counters.received.objects = 7;

  auto out = format(makeResult(counters, "svc"), /*omitMetadata=*/true);

  EXPECT_EQ(out.find("# HELP"), std::string::npos) << out;
  EXPECT_EQ(out.find("# TYPE"), std::string::npos) << out;
  EXPECT_NE(
      out.find(
          "moqx_track_objects_received_total{service=\"svc\",namespace=\"ns\",track=\"track\"} 7\n"
      ),
      std::string::npos
  ) << out;
}

TEST(TrackMetricsFormatTest, EscapesServiceLabel) {
  auto out = format(makeResult(stats::TrackCounters{}, "a\"b\\c"));

  EXPECT_NE(out.find(R"(service="a\"b\\c")"), std::string::npos) << out;
}

} // namespace openmoq::moqx::admin
