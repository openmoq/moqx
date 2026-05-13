/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "logging/MLogCleaner.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <vector>

#include <folly/logging/xlog.h>

namespace openmoq::moqx::logging {

namespace {
constexpr uint64_t kBytesPerMb = 1024ULL * 1024ULL;
constexpr uint64_t kSecondsPerDay = 24ULL * 60ULL * 60ULL;
} // namespace

MLogCleaner::MLogCleaner(
    std::string dir,
    std::optional<uint32_t> maxAgeDays,
    std::optional<uint64_t> maxDirMb)
    : dir_(std::move(dir)) {
  if (maxAgeDays) {
    maxAge_ = std::chrono::seconds(*maxAgeDays * kSecondsPerDay);
  }
  if (maxDirMb) {
    maxDirBytes_ = *maxDirMb * kBytesPerMb;
  }
}

void MLogCleaner::cleanup() {
  namespace fs = std::filesystem;
  using namespace std::chrono;

  if (dir_.empty()) {
    return;
  }
  if (!maxAge_ && !maxDirBytes_) {
    return;
  }

  std::error_code ec;
  if (!fs::is_directory(dir_, ec)) {
    XLOG(WARN) << "mlog cleanup: directory not found or inaccessible: " << dir_;
    return;
  }

  const auto now = system_clock::now();

  struct FileEntry {
    fs::path path;
    fs::file_time_type mtime;
    uintmax_t size;
  };

  std::vector<FileEntry> remaining;
  uint64_t totalSize = 0;

  for (const auto& entry : fs::directory_iterator(dir_, ec)) {
    if (!entry.is_regular_file(ec) || ec) {
      if (ec) {
        XLOG(WARN) << "mlog cleanup: failed to check file type " << entry.path() << ": "
                   << ec.message();
      }
      ec.clear();
      continue;
    }
    if (entry.path().extension() != ".mlog") {
      continue;
    }

    const uintmax_t size = entry.file_size(ec);
    if (ec) {
      XLOG(WARN) << "mlog cleanup: failed to stat " << entry.path() << ": " << ec.message();
      ec.clear();
      continue;
    }
    const auto mtime = entry.last_write_time(ec);
    if (ec) {
      XLOG(WARN) << "mlog cleanup: failed to get mtime " << entry.path() << ": "
                 << ec.message();
      ec.clear();
      continue;
    }

    // Age-based deletion: remove immediately if beyond max age.
    if (maxAge_) {
      if (now - clock_cast<system_clock>(mtime) > *maxAge_) {
        fs::remove(entry.path(), ec);
        if (ec) {
          XLOG(WARN) << "mlog cleanup: failed to remove aged file "
                     << entry.path() << ": " << ec.message();
          ec.clear();
        }
        continue;
      }
    }

    remaining.push_back({entry.path(), mtime, size});
    totalSize += size;
  }

  // Size-based deletion: delete oldest files first until under the limit.
  if (maxDirBytes_ && totalSize > *maxDirBytes_) {
    std::sort(remaining.begin(), remaining.end(), [](const FileEntry& a, const FileEntry& b) {
      return a.mtime < b.mtime; // oldest first
    });

    for (const auto& f : remaining) {
      if (totalSize <= *maxDirBytes_) {
        break;
      }
      fs::remove(f.path, ec);
      if (ec) {
        XLOG(WARN) << "mlog cleanup: failed to remove oversized file " << f.path << ": "
                   << ec.message();
        ec.clear();
        continue;
      }
      totalSize -= f.size;
    }
  }
}

} // namespace openmoq::moqx::logging
