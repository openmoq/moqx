#pragma once

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace openmoq::moqx::config::test {

// RAII helper: writes YAML content to a unique temp file, removes it on destruction.
class TempYamlFile {
public:
  explicit TempYamlFile(std::string_view content) {
    static std::atomic<int> counter{0};
    path_ = std::filesystem::temp_directory_path() /
            ("moqx_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter++) + ".yaml");
    std::ofstream ofs(path_);
    ofs << content;
  }
  ~TempYamlFile() { std::filesystem::remove(path_); }

  TempYamlFile(const TempYamlFile&) = delete;
  TempYamlFile& operator=(const TempYamlFile&) = delete;

  std::string path() const { return path_.string(); }

private:
  std::filesystem::path path_;
};

} // namespace openmoq::moqx::config::test
