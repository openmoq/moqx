#pragma once

#include <fstream>
#include <string>
#include <string_view>

#include <folly/testing/TestUtil.h>

namespace openmoq::o_rly::config::test {

class TempYamlFile {
public:
  explicit TempYamlFile(std::string_view content) {
    auto filePath = dir_.path() / "config.yaml";
    std::ofstream ofs(filePath);
    ofs << content;
    path_ = filePath.string();
  }

  TempYamlFile(const TempYamlFile&) = delete;
  TempYamlFile& operator=(const TempYamlFile&) = delete;

  std::string path() const { return path_; }

private:
  folly::test::TemporaryDirectory dir_{"o_rly_config_test"};
  std::string path_;
};

} // namespace openmoq::o_rly::config::test
