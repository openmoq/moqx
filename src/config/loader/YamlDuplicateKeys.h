/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>

namespace openmoq::moqx::config {

/// Rejects a YAML document that repeats a key within one mapping, or uses a
/// collection as a mapping key.
/// yaml-cpp keeps every entry of a repeated key; the reflect step then silently
/// drops or keeps the extras.
/// Throws std::runtime_error naming the key and line.
/// Throws YAML::Exception on malformed YAML.
void rejectDuplicateKeys(const std::string& content);

} // namespace openmoq::moqx::config
