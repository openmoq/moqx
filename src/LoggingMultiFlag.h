/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace openmoq::moqx {

/**
 * Compose a folly logging config string from category and handler value
 * lists. Categories joined with `,`; handlers joined with `;`. If both
 * present the composite is `<cats>;<handlers>`; if only handlers, the
 * composite is `;<handlers>` (per folly's grammar). Empty values dropped.
 */
std::string combineLoggingValues(
    const std::vector<std::string_view>& categoryValues,
    const std::vector<std::string_view>& handlerValues
);

/**
 * Walk argv, collect every `--logging` and `--log-handler` value, compose
 * one composite via combineLoggingValues(), and replace every matching
 * argv slot with `--logging=<composite>`. gflags last-wins then picks the
 * composite; folly never sees the unknown `--log-handler` name.
 *
 * MUST be called before folly::Init.
 */
void combineLoggingArgs(int argc, char** argv);

} // namespace openmoq::moqx
