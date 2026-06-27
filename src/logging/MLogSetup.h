/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "config/Config.h"

#include <memory>

#include <folly/Expected.h>
#include <folly/executors/IOThreadPoolExecutor.h>
#include <moxygen/mlog/MLoggerFactory.h>

namespace openmoq::moqx::logging {

/**
 * Owns all runtime mlog resources: the logger factory and the background
 * write executor.
 */
struct MLogSetup {
  std::shared_ptr<moxygen::MLoggerFactory> factory;
  std::shared_ptr<folly::IOThreadPoolExecutor> executor;
};

/**
 * Constructs an MLogSetup from the resolved config.
 * Creates the mlog directory if it does not already exist.
 * Returns makeUnexpected(1) (exit code) on failure.
 */
folly::Expected<MLogSetup, int> setupMLog(const config::Config& config);

} // namespace openmoq::moqx::logging
