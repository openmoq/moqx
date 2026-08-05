/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SafeTrackName.h"

#include <gtest/gtest.h>

namespace openmoq::moqx {

TEST(SafeTrackNameTest, PassesThroughUnreservedBytes) {
  EXPECT_EQ(safeName("abcXYZ019_"), "abcXYZ019_");
}

TEST(SafeTrackNameTest, EncodesEverythingElseAsLowerHex) {
  EXPECT_EQ(safeName("a.b"), "a.2eb");
  EXPECT_EQ(safeName("a/b"), "a.2fb");
  EXPECT_EQ(safeName(" "), ".20");
  EXPECT_EQ(safeName("\n"), ".0a");
  EXPECT_EQ(safeName("\xff"), ".ff");
  EXPECT_EQ(safeName(std::string_view("\0", 1)), ".00");
}

// The separators are only unambiguous because '-' does not pass through.
TEST(SafeTrackNameTest, EncodesHyphen) {
  EXPECT_EQ(safeName("a-b"), "a.2db");
}

TEST(SafeTrackNameTest, JoinsNamespaceTuplesWithHyphen) {
  moxygen::TrackNamespace ns{{"conf", "room1", "layer0"}};
  EXPECT_EQ(safeName(ns), "conf-room1-layer0");
}

TEST(SafeTrackNameTest, EmptyNamespaceAndTuples) {
  EXPECT_EQ(safeName(moxygen::TrackNamespace{}), "");
  moxygen::TrackNamespace ns{{"", ""}};
  EXPECT_EQ(safeName(ns), "-");
}

TEST(SafeTrackNameTest, SeparatesTrackNameWithDoubleHyphen) {
  moxygen::FullTrackName ftn{moxygen::TrackNamespace{{"conf", "room1"}}, "video"};
  EXPECT_EQ(safeName(ftn), "conf-room1--video");
}

TEST(SafeTrackNameTest, RendersBinaryNamesReadablyWhereItCan) {
  moxygen::FullTrackName ftn{
      moxygen::TrackNamespace{{"conf.example.com", std::string("bin\xff\x01", 5)}},
      "video-1"
  };
  EXPECT_EQ(safeName(ftn), "conf.2eexample.2ecom-bin.ff.01--video.2d1");
}

// Names that differ only outside the pass-through set must not collapse onto
// one rendering; metric labels key on the result.
TEST(SafeTrackNameTest, DistinctNamesStayDistinct) {
  EXPECT_NE(safeName("\xc3\xa9"), safeName("\xc3\xa8"));
  EXPECT_NE(safeName("a-b"), safeName("a_b"));

  moxygen::TrackNamespace twoTuples{{"a", "b"}};
  moxygen::TrackNamespace oneTuple{{"a-b"}};
  EXPECT_NE(safeName(twoTuples), safeName(oneTuple));
}

TEST(SafeTrackNameTest, ParsesBytesBackToOriginal) {
  EXPECT_EQ(parseSafeBytes("abcXYZ019_"), "abcXYZ019_");
  EXPECT_EQ(parseSafeBytes("a.2eb"), "a.b");
  EXPECT_EQ(parseSafeBytes(".ff"), "\xff");
  EXPECT_EQ(parseSafeBytes(""), "");
  EXPECT_EQ(parseSafeBytes(std::string_view(".00")), std::string_view("\0", 1));
}

TEST(SafeTrackNameTest, ParsesEitherHexCase) {
  EXPECT_EQ(parseSafeBytes(".FF"), "\xff");
  EXPECT_EQ(parseSafeBytes(".aB"), "\xab");
}

TEST(SafeTrackNameTest, RejectsMalformedBytes) {
  EXPECT_FALSE(parseSafeBytes("a."));
  EXPECT_FALSE(parseSafeBytes("a.2"));
  EXPECT_FALSE(parseSafeBytes(".zz"));
  EXPECT_FALSE(parseSafeBytes("a b"));
  // Separators are not part of a component's alphabet.
  EXPECT_FALSE(parseSafeBytes("a-b"));
  // Neither a sign nor leading space may stand in for a hex digit.
  EXPECT_FALSE(parseSafeBytes(".-1"));
  EXPECT_FALSE(parseSafeBytes(". 1"));
  EXPECT_FALSE(parseSafeBytes(".0x"));
}

TEST(SafeTrackNameTest, ParsesNamespaceTuples) {
  auto ns = parseSafeNamespace("conf-room1-layer0");
  ASSERT_TRUE(ns);
  EXPECT_EQ(ns->trackNamespace, std::vector<std::string>({"conf", "room1", "layer0"}));

  auto empty = parseSafeNamespace("");
  ASSERT_TRUE(empty);
  EXPECT_TRUE(empty->trackNamespace.empty());

  auto emptyTuples = parseSafeNamespace("-");
  ASSERT_TRUE(emptyTuples);
  EXPECT_EQ(emptyTuples->trackNamespace, std::vector<std::string>({"", ""}));
}

TEST(SafeTrackNameTest, ParsesFullTrackName) {
  auto ftn = parseSafeFullTrackName("conf-room1--video");
  ASSERT_TRUE(ftn);
  EXPECT_EQ(ftn->trackNamespace.trackNamespace, std::vector<std::string>({"conf", "room1"}));
  EXPECT_EQ(ftn->trackName, "video");
}

TEST(SafeTrackNameTest, RejectsFullTrackNameWithoutSeparator) {
  EXPECT_FALSE(parseSafeFullTrackName("conf-room1"));
  EXPECT_FALSE(parseSafeFullTrackName(""));
}

// The last "--" wins, so a namespace ending in an empty tuple still splits
// where the encoder put the separator.
TEST(SafeTrackNameTest, ParsesEmptyTuplesAroundSeparator) {
  auto trailing = parseSafeFullTrackName("a---v");
  ASSERT_TRUE(trailing);
  EXPECT_EQ(trailing->trackNamespace.trackNamespace, std::vector<std::string>({"a", ""}));
  EXPECT_EQ(trailing->trackName, "v");

  auto emptyTrack = parseSafeFullTrackName("a--");
  ASSERT_TRUE(emptyTrack);
  EXPECT_EQ(emptyTrack->trackNamespace.trackNamespace, std::vector<std::string>({"a"}));
  EXPECT_EQ(emptyTrack->trackName, "");
}

TEST(SafeTrackNameTest, RoundTripsAwkwardNames) {
  const std::vector<moxygen::FullTrackName> names{
      {moxygen::TrackNamespace{{"conf.example.com", "room 1"}}, "video-1"},
      {moxygen::TrackNamespace{{"a", ""}}, ""},
      {moxygen::TrackNamespace{{std::string("\x00\xff-", 3)}}, std::string("\n\t", 2)},
      {moxygen::TrackNamespace{}, "solo"},
  };
  for (const auto& ftn : names) {
    auto parsed = parseSafeFullTrackName(safeName(ftn));
    ASSERT_TRUE(parsed) << safeName(ftn);
    EXPECT_EQ(parsed->trackNamespace.trackNamespace, ftn.trackNamespace.trackNamespace);
    EXPECT_EQ(parsed->trackName, ftn.trackName);
  }
}

} // namespace openmoq::moqx
