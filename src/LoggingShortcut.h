/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/Range.h>

#include <string>

namespace openmoq::moqx {

/**
 * Rewrite each comma- or semicolon-separated clause of a folly logging
 * config string so that a bare level token (e.g. "DBG2", "WARN") becomes
 * ".=<level>". Clauses that already contain '=' (full category=level
 * specs, default:async=true blocks, etc.) pass through unchanged.
 *
 * The bare-level shortcut is moqx-only ergonomics. folly's own
 * LogConfigParser requires `<category>=<level>` form; this layer
 * normalizes the operator-facing input before handing it to folly.
 *
 * Examples:
 *
 *     "DBG2"                                  -> ".=DBG2"
 *     "WARN"                                  -> ".=WARN"
 *     "DBG2,moxygen=DBG4"                     -> ".=DBG2,moxygen=DBG4"
 *     "WARN; default:async=true,sync_level=WARN"
 *                                             -> ".=WARN; default:async=true,sync_level=WARN"
 *     "moxygen=DBG4"                          -> "moxygen=DBG4"     (already has '=')
 *     ".=WARN; default:async=true"            -> ".=WARN; default:async=true"   (no change)
 *     ""                                      -> ""
 */
std::string normalizeLoggingConfig(folly::StringPiece raw);

/**
 * Walk argv looking for `--logging=VALUE` and `--logging VALUE` forms;
 * for each, apply normalizeLoggingConfig() to the value and point the
 * argv slot at a stable-storage transformed string.
 *
 * MUST be called before folly::Init, because folly's gflag parser does
 * not accept the bare-level shortcut: a value like `DBG2` would reach
 * parseLogConfig() without an '=' and throw LogConfigParseError.
 */
void normalizeLoggingArgv(int argc, char** argv);

} // namespace openmoq::moqx
