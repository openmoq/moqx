/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "config/Config.h"
#include "logging/MLogCleaner.h"

#include <functional>
#include <memory>

#include <folly/Expected.h>
#include <folly/executors/IOThreadPoolExecutor.h>
#include <folly/io/async/EventBase.h>
#include <moxygen/mlog/MLoggerFactory.h>

namespace openmoq::moqx::logging {

/**
 * Owns all runtime mlog resources: the logger factory, the background write
 * executor, the optional cleaner, and the self-rescheduling cleanup lambda.
 */
struct MLogSetup {
  std::shared_ptr<moxygen::MLoggerFactory> factory;
  std::shared_ptr<folly::IOThreadPoolExecutor> executor;
  std::shared_ptr<MLogCleaner> cleaner;
  uint32_t cleanupIntervalMs{0};
  // Kept alive for the duration of the event loop; holds the self-rescheduling lambda.
  std::shared_ptr<std::function<void()>> cleanupSchedule;

  /**
   * Runs an immediate cleanup pass on the executor, then schedules periodic
   * passes via evb. No-op if cleaner or executor are absent.
   */
  void scheduleCleanup(folly::EventBase& evb);
};

/**
 * Constructs an MLogSetup from the resolved config.
 * Creates the mlog directory if it does not already exist.
 * Returns makeUnexpected(1) (exit code) on failure.
 */
folly::Expected<MLogSetup, int> setupMLog(const config::Config& config);

} // namespace openmoq::moqx::logging
