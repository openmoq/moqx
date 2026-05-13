/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace openmoq::moqx::logging {

/**
 * MLogCleaner enforces retention limits on a directory of .mlog files.
 *
 * Two independent limits can be set, both applied on each cleanup() call:
 *
 *   1. Age limit (maxAgeDays): any .mlog file whose last-write time is older
 *      than the configured number of days is deleted unconditionally.
 *
 *   2. Size limit (maxDirMb): after age-based deletion, if the sum of
 *      remaining .mlog file sizes still exceeds the limit, files are deleted
 *      in oldest-first order until the directory is under the limit.
 *
 * cleanup() is designed to be called:
 *   - once at startup (to clean up files left by previous relay runs), and
 *   - periodically during the relay's lifetime (interval is configurable) to
 *     prevent unbounded growth.
 *
 * cleanup() is not thread-safe; callers must serialise calls externally (e.g.
 * by always posting to the same single-threaded mlog executor).
 */
class MLogCleaner {
 public:
  MLogCleaner(
      std::string dir,
      std::optional<uint32_t> maxAgeDays,
      std::optional<uint64_t> maxDirMb);

  /**
   * Run one cleanup pass.  Logs warnings on filesystem errors but never
   * throws; partial progress (some files deleted) is acceptable.
   */
  void cleanup();

 private:
  std::string dir_;
  std::optional<std::chrono::seconds> maxAge_; // derived from maxAgeDays
  std::optional<uint64_t> maxDirBytes_;        // derived from maxDirMb
};

} // namespace openmoq::moqx::logging
