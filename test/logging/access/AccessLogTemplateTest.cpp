/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "logging/access/AccessLogTemplate.h"
#include "logging/access/TemplateEncoder.h"

#include <set>

#include <gtest/gtest.h>

namespace openmoq::moqx::logging {
namespace {

using namespace std::chrono_literals;

// 2026-10-09T17:02:10.020Z
constexpr auto kTime = std::chrono::system_clock::time_point(1791565330020ms);

std::shared_ptr<AccessLogSessionInfo> makeSession() {
  auto info = std::make_shared<AccessLogSessionInfo>();
  info->session = "9c01";
  info->openedBy = AccessLogOpenedBy::Peer;
  info->service = "live";
  info->peerTrunc = "203.0.113.0/24";
  info->peerClass = "origin-ingest";
  info->version = 16;
  return info;
}

std::string render(std::string_view text, const AccessLogRecord& record, uint64_t seq = 0) {
  auto tmpl = AccessLogTemplate::parse(text);
  EXPECT_TRUE(tmpl.hasValue()) << (tmpl.hasError() ? tmpl.error() : "");
  if (tmpl.hasError()) {
    return {};
  }
  AccessLogRelayInfo relayInfo;
  relayInfo.relayId = "edge-1";
  std::string out;
  tmpl->render(record, AccessLogRenderContext{relayInfo, seq}, out);
  return out;
}

AccessLogRecord setupRecord() {
  AccessLogRecord r;
  r.event = AccessLogEvent::SETUP;
  r.time = kTime;
  r.session = makeSession();
  r.status = AccessLogStatus::Ok;
  r.latencyMs = 1;
  return r;
}

TEST(AccessLogTemplateTest, RendersSessionLine) {
  EXPECT_EQ(
      render(
          "$schema $time $event $session $opened $dir $peer_trunc $peer_class $service $version "
          "$status $latency_ms $error_class",
          setupRecord()
      ),
      "moqx.v1 2026-10-09T17:02:10.020Z SETUP 9c01 accepted - 203.0.113.0/24 origin-ingest live 16 "
      "ok 1 -"
  );
}

TEST(AccessLogTemplateTest, RendersRequestLineWithSafeNames) {
  AccessLogRecord r;
  r.event = AccessLogEvent::SUBSCRIBE;
  r.time = kTime;
  r.session = makeSession();
  r.dir = AccessLogDir::Tx;
  r.requestID = 1;
  r.ftn = std::make_shared<moxygen::FullTrackName>(
      moxygen::FullTrackName{moxygen::TrackNamespace({"live", "ch1"}), "video"}
  );
  r.status = AccessLogStatus::Ok;
  r.latencyMs = 38;
  EXPECT_EQ(
      render("$event $session $dir $req $ftn $namespace $track $ns_prefix $status $latency_ms", r),
      "SUBSCRIBE 9c01 tx 1 live-ch1--video live-ch1 video - ok 38"
  );
}

TEST(AccessLogTemplateTest, NamespaceRequestRendersPrefixOnly) {
  AccessLogRecord r;
  r.event = AccessLogEvent::PUBLISH_NAMESPACE;
  r.session = makeSession();
  r.dir = AccessLogDir::Rx;
  r.requestID = 0;
  r.ftn = std::make_shared<moxygen::FullTrackName>(
      moxygen::FullTrackName{moxygen::TrackNamespace({"live", "ch1"}), ""}
  );
  r.namespaceOnly = true;
  EXPECT_EQ(render("$event $ftn $ns_prefix $track", r), "PUBLISH_NAMESPACE live-ch1 live-ch1 -");
}

TEST(AccessLogTemplateTest, DistinguishesUnsetAndZero) {
  auto r = setupRecord();
  r.latencyMs = 0;
  // latency_ms is a real zero, and requestID does not apply to a session record.
  EXPECT_EQ(render("$event $latency_ms $req $duration_ms", r), "SETUP 0 - -");
}

TEST(AccessLogTemplateTest, RecordWithoutSessionRendersSessionFieldsAsDash) {
  AccessLogRecord r;
  r.event = AccessLogEvent::LOG_DROPPED;
  r.time = kTime;
  r.dropped = 12;
  EXPECT_EQ(
      render("$event $session $peer $version $dropped $relay_id $seq", r, 7),
      "LOG_DROPPED - - - 12 edge-1 7"
  );
}

TEST(AccessLogTemplateTest, EscapesFreeText) {
  auto r = setupRecord();
  r.status = AccessLogStatus::Rejected;
  r.reason = "bad \"token\"\\\n";
  EXPECT_EQ(render("$event $reason", r), "SETUP bad\\x20\\x22token\\x22\\x5c\\x0a");
}

TEST(AccessLogTemplateTest, RendersEpochMillis) {
  EXPECT_EQ(render("$event $msec", setupRecord()), "SETUP 1791565330.020");
}

TEST(AccessLogTemplateTest, BracedVariableJoinsLiteralText) {
  EXPECT_EQ(render("${event}:${session}x", setupRecord()), "SETUP:9c01x");
}

TEST(AccessLogTemplateTest, RejectsUnknownVariable) {
  auto tmpl = AccessLogTemplate::parse("$event $nope");
  ASSERT_TRUE(tmpl.hasError());
  EXPECT_EQ(tmpl.error(), "unknown variable $nope");
}

TEST(AccessLogTemplateTest, RejectsTemplateWithoutEvent) {
  auto tmpl = AccessLogTemplate::parse("$time $session");
  ASSERT_TRUE(tmpl.hasError());
  EXPECT_EQ(tmpl.error(), "template must include $event");
}

TEST(AccessLogTemplateTest, RejectsClassSpecificVariableBeforeEvent) {
  auto tmpl = AccessLogTemplate::parse("$time $req $event");
  ASSERT_TRUE(tmpl.hasError());
  EXPECT_EQ(tmpl.error(), "$req must come after $event");
  EXPECT_TRUE(AccessLogTemplate::parse("$time $session $event $req").hasValue());
}

TEST(AccessLogTemplateTest, ComparesPrefixesThroughEvent) {
  auto a = *AccessLogTemplate::parse("$schema $time $event $req");
  auto b = *AccessLogTemplate::parse("$schema $time $event $session $duration_ms");
  auto c = *AccessLogTemplate::parse("$time $schema $event $req");
  EXPECT_TRUE(a.sharesPrefixWith(b));
  EXPECT_FALSE(a.sharesPrefixWith(c));
  EXPECT_EQ(a.prefixText(), "$schema $time $event");
}

TEST(AccessLogTemplateTest, RejectsLineBreaksAndUppercaseNames) {
  auto broken = AccessLogTemplate::parse("$event\n$session");
  ASSERT_TRUE(broken.hasError());
  EXPECT_EQ(broken.error(), "line break at offset 6");
  auto upper = AccessLogTemplate::parse("$Event");
  ASSERT_TRUE(upper.hasError());
  EXPECT_EQ(upper.error(), "unknown variable $Event");
  // Other control bytes are allowed as delimiters.
  EXPECT_TRUE(AccessLogTemplate::parse("$event\x01$session").hasValue());
}

TEST(AccessLogTemplateTest, RejectsDanglingDollar) {
  EXPECT_TRUE(AccessLogTemplate::parse("$event $").hasError());
  EXPECT_TRUE(AccessLogTemplate::parse("$event ${session").hasError());
  EXPECT_TRUE(AccessLogTemplate::parse("$event ${}").hasError());
}

TEST(AccessLogTemplateTest, EventNamesRoundTripAndMapToClasses) {
  for (uint8_t i = 0; i <= uint8_t(AccessLogEvent::LOG_DROPPED); ++i) {
    auto event = AccessLogEvent(i);
    EXPECT_EQ(parseAccessLogEvent(accessLogEventName(event)), event);
  }
  EXPECT_EQ(accessLogClass(AccessLogEvent::SESSION_END), AccessLogClass::Session);
  EXPECT_EQ(accessLogClass(AccessLogEvent::FETCH), AccessLogClass::Request);
  EXPECT_EQ(accessLogClass(AccessLogEvent::SUB_STATS), AccessLogClass::Periodic);
  EXPECT_EQ(accessLogClass(AccessLogEvent::LOG_DROPPED), AccessLogClass::Internal);
  EXPECT_EQ(parseAccessLogClass("request"), AccessLogClass::Request);
  EXPECT_FALSE(parseAccessLogEvent("subscribe").has_value());
}

TEST(AccessLogTemplateTest, VariableNamesAreUnique) {
  std::set<std::string_view> names;
  for (const auto& var : accessLogVars()) {
    EXPECT_TRUE(names.insert(var.name).second) << var.name;
    EXPECT_NE(var.classes, 0) << var.name;
  }
}

TEST(AccessLogTemplateTest, EmptyNamespaceKeepsItsColumn) {
  AccessLogRecord r;
  r.event = AccessLogEvent::SUBSCRIBE_NAMESPACE;
  r.ftn = std::make_shared<moxygen::FullTrackName>();
  r.namespaceOnly = true;
  EXPECT_EQ(render("$event $ftn $ns_prefix $req", r), "SUBSCRIBE_NAMESPACE \"\" \"\" -");
}

TEST(TemplateEncoderTest, AlignPrefixesGivesLogDroppedTheSharedPrefix) {
  auto encoder = defaultTemplateEncoder();
  encoder.setClassTemplate(AccessLogClass::Request, *AccessLogTemplate::parse("$time $event $req"));
  // The default session template still starts with $schema.
  EXPECT_TRUE(encoder.alignPrefixes().has_value());
  encoder.setClassTemplate(
      AccessLogClass::Session,
      *AccessLogTemplate::parse("$time $event $session")
  );
  EXPECT_FALSE(encoder.alignPrefixes().has_value());

  AccessLogRecord r;
  r.event = AccessLogEvent::LOG_DROPPED;
  r.time = kTime;
  r.dropped = 4;
  AccessLogRelayInfo relayInfo;
  std::string out;
  encoder.encode(r, AccessLogRenderContext{relayInfo, 0}, out);
  EXPECT_EQ(out, "2026-10-09T17:02:10.020Z LOG_DROPPED 4");

  // A class without a template prints the shared prefix.
  r.event = AccessLogEvent::SUB_STATS;
  out.clear();
  encoder.encode(r, AccessLogRenderContext{relayInfo, 0}, out);
  EXPECT_EQ(out, "2026-10-09T17:02:10.020Z SUB_STATS");
}

} // namespace
} // namespace openmoq::moqx::logging
