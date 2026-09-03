/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// folly::test::TemporaryDirectory/TemporaryFile were tried and rejected: they
// live in folly_testing_test_util, which prebuilt deps ship compiled against
// the build distro's boost (versioned boost::regex symbols), so linking fails
// on hosts whose boost version differs from the tarball's.

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unistd.h>

namespace openmoq::moqx::test {

namespace detail {
// Unique per-process temp path; the counter is shared by all helpers here.
inline std::filesystem::path uniqueTempPath(const std::string& prefix, std::string_view suffix) {
  static std::atomic<int> counter{0};
  return std::filesystem::temp_directory_path() / (prefix + std::to_string(::getpid()) + "_" +
                                                   std::to_string(counter++) + std::string(suffix));
}
} // namespace detail

// RAII temp file: bytes written to a unique path with the given suffix,
// removed on destruction.
class TempFile {
public:
  TempFile(std::string_view bytes, std::string_view suffix)
      : path_(detail::uniqueTempPath("moqx_test_", suffix)) {
    std::ofstream ofs(path_, std::ios::binary);
    ofs.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  ~TempFile() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;

  std::string path() const { return path_.string(); }

private:
  std::filesystem::path path_;
};

// RAII temp directory: unique path, recursively removed on destruction.
class TempDir {
public:
  TempDir() : path_(detail::uniqueTempPath("moqx_test_dir_", "")) {
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  std::string path() const { return path_.string(); }

  std::string writeFile(const std::string& filename, const std::string& bytes) const {
    auto p = path_ / filename;
    std::ofstream ofs(p, std::ios::binary);
    ofs.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return p.string();
  }
  void removeFile(const std::string& filename) const { std::filesystem::remove(path_ / filename); }

private:
  std::filesystem::path path_;
};

} // namespace openmoq::moqx::test
