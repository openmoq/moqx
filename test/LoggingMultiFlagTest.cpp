/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LoggingMultiFlag.h"

#include <folly/Range.h>
#include <folly/logging/LogConfigParser.h>
#include <gtest/gtest.h>

#include <vector>

using openmoq::moqx::combineLoggingArgs;
using openmoq::moqx::combineLoggingValues;

namespace {
std::vector<folly::StringPiece> pieces(std::initializer_list<folly::StringPiece> ps) {
  return {ps.begin(), ps.end()};
}
} // namespace

// ─── combineLoggingValues — the pure helper ──────────────────────────────────

TEST(CombineLoggingValues, BothEmpty) {
  EXPECT_EQ("", combineLoggingValues({}, {}));
}

TEST(CombineLoggingValues, OnlyCategories) {
  EXPECT_EQ(".=INFO", combineLoggingValues(pieces({".=INFO"}), {}));
  EXPECT_EQ(".=INFO,moxygen=DBG4", combineLoggingValues(pieces({".=INFO", "moxygen=DBG4"}), {}));
  EXPECT_EQ(
      ".=INFO,moxygen=DBG4,quic=WARN",
      combineLoggingValues(pieces({".=INFO", "moxygen=DBG4", "quic=WARN"}), {})
  );
}

TEST(CombineLoggingValues, OnlyHandlers) {
  // Handlers-only produces a leading `;` per folly's grammar
  // (empty categories block, then the handler block).
  EXPECT_EQ(";default:async=false", combineLoggingValues({}, pieces({"default:async=false"})));
  EXPECT_EQ(
      ";default:async=true,sync_level=WARN;extra=stuff",
      combineLoggingValues({}, pieces({"default:async=true,sync_level=WARN", "extra=stuff"}))
  );
}

TEST(CombineLoggingValues, BothPresent) {
  EXPECT_EQ(
      ".=INFO;default:async=false",
      combineLoggingValues(pieces({".=INFO"}), pieces({"default:async=false"}))
  );
  EXPECT_EQ(
      ".=INFO,moxygen=DBG4;default:async=true,sync_level=WARN",
      combineLoggingValues(
          pieces({".=INFO", "moxygen=DBG4"}),
          pieces({"default:async=true,sync_level=WARN"})
      )
  );
}

TEST(CombineLoggingValues, MultipleHandlersJoinWithSemicolon) {
  EXPECT_EQ(
      ".=INFO;default:async=false;extra=stuff",
      combineLoggingValues(pieces({".=INFO"}), pieces({"default:async=false", "extra=stuff"}))
  );
}

TEST(CombineLoggingValues, EmptyInputsAreDropped) {
  EXPECT_EQ(
      ".=INFO,moxygen=DBG4",
      combineLoggingValues(pieces({".=INFO", "", "moxygen=DBG4"}), {})
  );
  EXPECT_EQ(
      ".=INFO;default:async=false",
      combineLoggingValues(pieces({".=INFO", ""}), pieces({"", "default:async=false"}))
  );
}

TEST(CombineLoggingValues, OutputIsFollyParseable) {
  // Property: combined output is accepted by folly's actual parser
  // in every shape the multi-flag combiner can produce.
  struct Case {
    std::vector<folly::StringPiece> cats;
    std::vector<folly::StringPiece> handlers;
  };
  const std::vector<Case> kCases = {
      {{}, {}},
      {pieces({".=INFO"}), {}},
      {pieces({"INFO"}), {}}, // bare-level — folly accepts natively
      {pieces({".=INFO", "moxygen=DBG4"}), {}},
      {{}, pieces({"default:async=false"})},
      {pieces({".=INFO"}), pieces({"default:async=false"})},
      {pieces({"WARN"}), pieces({"default:async=true,sync_level=WARN"})},
      {pieces({".=INFO", "moxygen=DBG4", "quic=WARN"}),
       pieces({"default:async=false", "extra=stuff"})},
  };
  for (const auto& c : kCases) {
    auto combined = combineLoggingValues(c.cats, c.handlers);
    EXPECT_NO_THROW(folly::parseLogConfig(combined)) << "combined='" << combined << "'";
  }
}

// ─── combineLoggingArgs — the argv mutator ───────────────────────────────────

TEST(CombineLoggingArgs, NoLoggingFlagsIsNoop) {
  char arg0[] = "moqx";
  char arg1[] = "--config=c.yaml";
  char* argv[] = {arg0, arg1};
  combineLoggingArgs(2, argv);
  EXPECT_STREQ("--config=c.yaml", argv[1]);
}

TEST(CombineLoggingArgs, SingleLoggingFlagUnchanged) {
  // One --logging, no --log-handler — gflags already does the right
  // thing; we bail to avoid touching the slot.
  char arg0[] = "moqx";
  char arg1[] = "--logging=.=INFO";
  char* argv[] = {arg0, arg1};
  combineLoggingArgs(2, argv);
  EXPECT_STREQ("--logging=.=INFO", argv[1]);
}

TEST(CombineLoggingArgs, MultipleLoggingFlagsCombineWithComma) {
  char arg0[] = "moqx";
  char arg1[] = "--logging=.=INFO";
  char arg2[] = "--logging=moxygen=DBG4";
  char arg3[] = "--logging=quic=WARN";
  char* argv[] = {arg0, arg1, arg2, arg3};
  combineLoggingArgs(4, argv);
  EXPECT_STREQ("--logging=.=INFO,moxygen=DBG4,quic=WARN", argv[1]);
  EXPECT_STREQ("--logging=.=INFO,moxygen=DBG4,quic=WARN", argv[2]);
  EXPECT_STREQ("--logging=.=INFO,moxygen=DBG4,quic=WARN", argv[3]);
}

TEST(CombineLoggingArgs, LoggingPlusLogHandlerComposesWithSemicolon) {
  // The whole point of --log-handler: handler block without
  // shell-quoting the `;`.
  char arg0[] = "moqx";
  char arg1[] = "--logging=.=INFO";
  char arg2[] = "--log-handler=default:async=false";
  char* argv[] = {arg0, arg1, arg2};
  combineLoggingArgs(3, argv);
  EXPECT_STREQ("--logging=.=INFO;default:async=false", argv[1]);
  // The --log-handler slot is rewritten to --logging=<composite> too,
  // so gflags never sees the unrecognized flag name.
  EXPECT_STREQ("--logging=.=INFO;default:async=false", argv[2]);
}

TEST(CombineLoggingArgs, LogHandlerAloneProducesLeadingSemicolon) {
  // Only --log-handler, no --logging — composite has empty cats block.
  // folly's parser accepts this shape (";handlers").
  char arg0[] = "moqx";
  char arg1[] = "--log-handler=default:async=false";
  char* argv[] = {arg0, arg1};
  combineLoggingArgs(2, argv);
  EXPECT_STREQ("--logging=;default:async=false", argv[1]);
}

TEST(CombineLoggingArgs, MultipleEverythingCombined) {
  char arg0[] = "moqx";
  char arg1[] = "--logging=.=INFO";
  char arg2[] = "--logging=moxygen=DBG4";
  char arg3[] = "--log-handler=default:async=true,sync_level=WARN";
  char arg4[] = "--log-handler=extra=stuff";
  char* argv[] = {arg0, arg1, arg2, arg3, arg4};
  combineLoggingArgs(5, argv);
  const char* kExpected = "--logging=.=INFO,moxygen=DBG4;"
                          "default:async=true,sync_level=WARN;extra=stuff";
  EXPECT_STREQ(kExpected, argv[1]);
  EXPECT_STREQ(kExpected, argv[2]);
  EXPECT_STREQ(kExpected, argv[3]);
  EXPECT_STREQ(kExpected, argv[4]);
}

TEST(CombineLoggingArgs, SeparatedFormForLogging) {
  // --logging X (no equals)
  char arg0[] = "moqx";
  char arg1[] = "--logging";
  char arg2[] = ".=INFO";
  char arg3[] = "--logging";
  char arg4[] = "moxygen=DBG4";
  char* argv[] = {arg0, arg1, arg2, arg3, arg4};
  combineLoggingArgs(5, argv);
  EXPECT_STREQ("--logging", argv[1]);
  EXPECT_STREQ(".=INFO,moxygen=DBG4", argv[2]);
  EXPECT_STREQ("--logging", argv[3]);
  EXPECT_STREQ(".=INFO,moxygen=DBG4", argv[4]);
}

TEST(CombineLoggingArgs, SeparatedFormForLogHandler) {
  // --log-handler X form: flag-name slot must be renamed to --logging so
  // gflags doesn't reject the unknown flag.
  char arg0[] = "moqx";
  char arg1[] = "--logging=.=INFO";
  char arg2[] = "--log-handler";
  char arg3[] = "default:async=false";
  char* argv[] = {arg0, arg1, arg2, arg3};
  combineLoggingArgs(4, argv);
  EXPECT_STREQ("--logging=.=INFO;default:async=false", argv[1]);
  EXPECT_STREQ("--logging", argv[2]);
  EXPECT_STREQ(".=INFO;default:async=false", argv[3]);
}

TEST(CombineLoggingArgs, LeavesUnrelatedFlagsAlone) {
  char arg0[] = "moqx";
  char arg1[] = "--logging=.=INFO";
  char arg2[] = "--config=c.yaml";
  char arg3[] = "--log-handler=default:async=false";
  char arg4[] = "--strict_config";
  char* argv[] = {arg0, arg1, arg2, arg3, arg4};
  combineLoggingArgs(5, argv);
  EXPECT_STREQ("--logging=.=INFO;default:async=false", argv[1]);
  EXPECT_STREQ("--config=c.yaml", argv[2]);
  EXPECT_STREQ("--logging=.=INFO;default:async=false", argv[3]);
  EXPECT_STREQ("--strict_config", argv[4]);
}

TEST(CombineLoggingArgs, TrailingFlagWithNoValueDoesNotCrash) {
  // --logging or --log-handler as last arg, no value: malformed CLI,
  // shouldn't be counted (bail-early path or harmlessly skipped).
  char arg0[] = "moqx";
  char arg1[] = "--logging=.=INFO";
  char arg2[] = "--log-handler";
  char* argv[] = {arg0, arg1, arg2};
  combineLoggingArgs(3, argv);
  // Only one valid --logging recognized, no --log-handler value;
  // the single-flag bail leaves argv unchanged.
  EXPECT_STREQ("--logging=.=INFO", argv[1]);
  EXPECT_STREQ("--log-handler", argv[2]);
}
