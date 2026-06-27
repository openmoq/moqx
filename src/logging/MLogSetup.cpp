/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "logging/MLogSetup.h"

#include <filesystem>

#include <folly/executors/thread_factory/NamedThreadFactory.h>
#include <folly/logging/xlog.h>
#include <moxygen/mlog/FileMLoggerFactory.h>
#include <moxygen/mlog/SamplingMLoggerFactory.h>

namespace openmoq::moqx::logging {

void MLogSetup::scheduleCleanup(folly::EventBase& evb) {
  if (!cleaner || !executor) {
    return;
  }
  // Startup pass — run immediately on the mlog executor.
  executor->add([c = cleaner] { c->cleanup(); });
  // Periodic pass — schedule via the main event base, post work to executor.
  cleanupSchedule = std::make_shared<std::function<void()>>();
  std::weak_ptr<std::function<void()>> weak = cleanupSchedule;
  *cleanupSchedule = [&evb, c = cleaner, exec = executor, weak, ms = cleanupIntervalMs]() {
    evb.runAfterDelay(
        [c, exec, weak]() {
          exec->add([c] { c->cleanup(); });
          if (auto fn = weak.lock()) {
            (*fn)();
          }
        },
        ms);
  };
  (*cleanupSchedule)();
}

folly::Expected<MLogSetup, int> setupMLog(const config::Config& config) {
  MLogSetup result;
  if (!config.logging || !config.logging->mlog) {
    return result;
  }
  const auto& mcfg = *config.logging->mlog;
  if (mcfg.dir.empty()) {
    return result;
  }

  std::error_code ec;
  std::filesystem::create_directories(mcfg.dir, ec);
  if (ec) {
    XLOG(ERR) << "Failed to create mlog directory '" << mcfg.dir << "': " << ec.message();
    return folly::makeUnexpected(1);
  }

  auto baseFactory =
      std::make_shared<moxygen::DirMLoggerFactory>(mcfg.dir, moxygen::VantagePoint::SERVER);
  result.executor = std::make_shared<folly::IOThreadPoolExecutor>(
      1, std::make_shared<folly::NamedThreadFactory>("moqx-mlog")
  );
  baseFactory->setWriteExecutor(result.executor);
  if (mcfg.sampleRate < 1.0f) {
    result.factory =
        std::make_shared<moxygen::SamplingMLoggerFactory>(baseFactory, mcfg.sampleRate);
  } else {
    result.factory = std::move(baseFactory);
  }

  if (mcfg.maxAgeDays || mcfg.maxDirMb) {
    result.cleaner =
        std::make_shared<MLogCleaner>(mcfg.dir, mcfg.maxAgeDays, mcfg.maxDirMb);
    result.cleanupIntervalMs = mcfg.cleanupIntervalSecs * 1000;
  }

  return result;
}

} // namespace openmoq::moqx::logging
