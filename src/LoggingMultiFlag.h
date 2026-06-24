/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/Range.h>

#include <string>
#include <vector>

namespace openmoq::moqx {

/**
 * Compose a folly logging config string from separate category and
 * handler value lists:
 *
 *   - categoryValues are joined with `,` to form the categories block
 *   - handlerValues are joined with `;` to form the handler block
 *   - if both are present, the composite is `<cats>;<handlers>`
 *   - if only one is present, the composite is just that one (with a
 *     leading `;` for handlers-only, per folly's grammar)
 *
 * Empty values in either list are silently dropped.
 *
 *   combineLoggingValues({".=INFO", "moxygen=DBG4"}, {})
 *     -> ".=INFO,moxygen=DBG4"
 *   combineLoggingValues({".=INFO"}, {"default:async=false"})
 *     -> ".=INFO;default:async=false"
 *   combineLoggingValues({}, {"default:async=false"})
 *     -> ";default:async=false"
 *   combineLoggingValues({}, {})
 *     -> ""
 */
std::string combineLoggingValues(
    const std::vector<folly::StringPiece>& categoryValues,
    const std::vector<folly::StringPiece>& handlerValues
);

/**
 * Walk argv collecting every `--logging` value (categories) and every
 * `--log-handler` value (handler configs), compose a single composite
 * via combineLoggingValues(), and replace every `--logging` /
 * `--log-handler` argv slot with the same `--logging=<composite>` form.
 *
 * This gives operators two unambiguous, accumulating flags so neither
 * needs shell-quoting for the `;` block boundary:
 *
 *     moqx --logging=.=INFO --logging=moxygen=DBG4
 *     moqx --logging=.=INFO --log-handler=default:async=false
 *     moqx --logging=.=INFO --logging=moxygen=DBG4 \
 *          --log-handler=default:async=true,sync_level=WARN
 *
 * Each `--logging` is a category spec; each `--log-handler` is a
 * handler config. Internally we still hand folly a single composite
 * string in the standard `<cats>;<handlers>` grammar.
 *
 * `--logging` already has a gflag definition in folly; `--log-handler`
 * does not — but since this function rewrites every `--log-handler`
 * argv slot to `--logging=<composite>` before folly::Init, gflags
 * never sees the unknown name.
 *
 * MUST be called before folly::Init.
 */
void combineLoggingArgs(int argc, char** argv);

} // namespace openmoq::moqx
