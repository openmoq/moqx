/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/io/IOBuf.h>

#include <cstdint>
#include <memory>

namespace openmoq::o_rly::test {

std::unique_ptr<folly::IOBuf> makeBuf(uint32_t size = 10);

} // namespace openmoq::o_rly::test
