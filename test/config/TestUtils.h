/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string_view>

#include "../util/TempDir.h"

namespace openmoq::moqx::config::test {

// TempFile with the ".yaml" suffix baked in.
struct TempYamlFile : moqx::test::TempFile {
  explicit TempYamlFile(std::string_view content) : TempFile(content, ".yaml") {}
};

} // namespace openmoq::moqx::config::test
