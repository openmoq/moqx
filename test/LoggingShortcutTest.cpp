/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LoggingShortcut.h"

#include <gtest/gtest.h>

using openmoq::moqx::normalizeLoggingConfig;

TEST(LoggingShortcut, BareLevelGetsRootPrefix) {
  EXPECT_EQ(".=DBG2", normalizeLoggingConfig("DBG2"));
  EXPECT_EQ(".=WARN", normalizeLoggingConfig("WARN"));
  EXPECT_EQ(".=INFO", normalizeLoggingConfig("INFO"));
  EXPECT_EQ(".=FATAL", normalizeLoggingConfig("FATAL"));
}

TEST(LoggingShortcut, FullCategorySpecPassesThrough) {
  EXPECT_EQ("moxygen=DBG4", normalizeLoggingConfig("moxygen=DBG4"));
  EXPECT_EQ(".=WARN", normalizeLoggingConfig(".=WARN"));
  EXPECT_EQ("quic.picoquic=DBG3", normalizeLoggingConfig("quic.picoquic=DBG3"));
}

TEST(LoggingShortcut, MixedShorthandAndCategorySpec) {
  // First clause is bare, second is full → only the first gets the prefix.
  EXPECT_EQ(".=DBG2,moxygen=DBG4",
            normalizeLoggingConfig("DBG2,moxygen=DBG4"));
  // Bare in middle position.
  EXPECT_EQ("moxygen=DBG4,.=WARN,quic=INFO",
            normalizeLoggingConfig("moxygen=DBG4,WARN,quic=INFO"));
}

TEST(LoggingShortcut, HandlerSettingsBlockPreservedAfterSemicolon) {
  // The `;` is the categories-vs-handler boundary; the categories block is
  // a single bare level, the handler block has its own `=` and so is left.
  EXPECT_EQ(".=WARN; default:async=true,sync_level=WARN",
            normalizeLoggingConfig("WARN; default:async=true,sync_level=WARN"));
  EXPECT_EQ(".=DBG2; default:async=false",
            normalizeLoggingConfig("DBG2; default:async=false"));
}

TEST(LoggingShortcut, NoChangeForFullForm) {
  EXPECT_EQ(".=WARN; default:async=true",
            normalizeLoggingConfig(".=WARN; default:async=true"));
  EXPECT_EQ("moqx=DBG4,moxygen=DBG3",
            normalizeLoggingConfig("moqx=DBG4,moxygen=DBG3"));
}

TEST(LoggingShortcut, EmptyInput) {
  EXPECT_EQ("", normalizeLoggingConfig(""));
}

TEST(LoggingShortcut, LeadingTrailingWhitespaceInClauseIsTrimmed) {
  // Bare-level clauses are trimmed so the rewritten ".=<level>" doesn't
  // carry stray whitespace inside the token.
  EXPECT_EQ(".=DBG2", normalizeLoggingConfig(" DBG2 "));
  // Passthrough clauses (already have `=`) preserve operator whitespace —
  // folly's parser handles internal whitespace fine, and preserving it
  // keeps the rewrite as close to the input as possible. Only the bare
  // clauses get any cleanup.
  EXPECT_EQ(".=DBG2, moxygen=DBG4 ",
            normalizeLoggingConfig(" DBG2 , moxygen=DBG4 "));
}

TEST(LoggingShortcut, EmptyClauseFromLeadingSeparatorIsHarmless) {
  // ",DBG2" parses as: empty clause (skip), then "DBG2" (rewrite).
  EXPECT_EQ(",.=DBG2", normalizeLoggingConfig(",DBG2"));
}
