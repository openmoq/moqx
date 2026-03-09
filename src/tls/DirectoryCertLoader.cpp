/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DirectoryCertLoader.h"

#include <algorithm>
#include <filesystem>

#include <folly/logging/xlog.h>

#include "CertUtils.h"

namespace openmoq::moqx::tls {

DirectoryCertLoader::DirectoryCertLoader(std::string certDir, std::string defaultCertIdentity)
    : certDir_(std::move(certDir)), defaultCertIdentity_(std::move(defaultCertIdentity)) {}

folly::Expected<LoadedCerts, std::string> DirectoryCertLoader::load() const {
  namespace fs = std::filesystem;

  // Collect .crt files sorted by name for deterministic ordering
  std::error_code ec;
  std::vector<fs::path> crtFiles;
  for (const auto& entry : fs::directory_iterator(certDir_, ec)) {
    if (entry.is_regular_file() && entry.path().extension() == ".crt") {
      crtFiles.push_back(entry.path());
    }
  }
  if (ec) {
    return folly::makeUnexpected("Failed to read directory " + certDir_ + ": " + ec.message());
  }

  std::sort(crtFiles.begin(), crtFiles.end());

  if (crtFiles.empty()) {
    return folly::makeUnexpected("No certificate pairs found in " + certDir_);
  }

  LoadedCerts result;
  for (const auto& crtPath : crtFiles) {
    auto keyPath = crtPath;
    keyPath.replace_extension(".key");

    auto cert = loadCertKeyPair(crtPath.string(), keyPath.string());
    if (cert.hasError()) {
      return folly::makeUnexpected(std::move(cert.error()));
    }

    auto identity = cert.value()->getIdentity();
    XLOG(INFO) << "Loaded certificate: " << identity << " from " << crtPath.filename().string();
    result.certs.push_back(LoadedCerts::Entry{
        .identity = std::move(identity),
        .cert = std::move(cert.value()),
    });
  }

  // Determine default identity
  if (!defaultCertIdentity_.empty()) {
    bool found = std::any_of(result.certs.begin(), result.certs.end(), [&](const auto& e) {
      return e.identity == defaultCertIdentity_;
    });
    if (!found) {
      return folly::makeUnexpected(
          "Specified default_cert identity '" + defaultCertIdentity_ +
          "' not found among loaded certificates"
      );
    }
    result.defaultIdentity = defaultCertIdentity_;
  } else {
    result.defaultIdentity = result.certs.front().identity;
  }

  return result;
}

} // namespace openmoq::moqx::tls
