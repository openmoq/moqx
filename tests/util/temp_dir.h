#pragma once

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace openmoq::moqx::test::util {

// RAII helper: Creates a temp directory to store temporary YAML config files or cert/key files.
// The directory is removed on destruction.
class TempDir {
public:
  TempDir()
      : dir_{std::filesystem::temp_directory_path() / ("moqx_test_" + std::to_string(::getpid()))} {
    std::filesystem::create_directories(dir_);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  ~TempDir() { std::filesystem::remove_all(dir_); }

  std::filesystem::path writeYaml(std::string_view content) {
    auto path = dir_ / ("config_" + std::to_string(yaml_counter_++) + ".yaml");
    std::ofstream ofs(path);
    ofs << content;
    return path;
  }

  void writeCert(const std::string& name, std::string_view certData, std::string_view keyData) {
    {
      std::ofstream ofs(dir_ / (name + ".crt"));
      ofs << certData;
    }
    {
      std::ofstream ofs(dir_ / (name + ".key"));
      ofs << keyData;
    }
  }

  void writeCertOnly(const std::string& name, std::string_view certData) {
    std::ofstream ofs(dir_ / (name + ".crt"));
    ofs << certData;
  }

  std::filesystem::path path() const { return dir_; }
  std::filesystem::path filePath(const std::string& filename) const { return dir_ / filename; }

private:
  std::filesystem::path dir_;
  size_t yaml_counter_ = 0;
};

} // namespace openmoq::moqx::test::util
