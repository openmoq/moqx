/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>

#include "TlsCertLoader.h"

namespace openmoq::moqx::tls {

/// Loads certificates from a specified directory. This directory is searched for pairs of files
/// with `.crt` and `.key` extensions; the `.crt` file is expected to be the certificate, and the
/// `.key` file the corresponding key file.
class DirectoryCertLoader : public TlsCertLoader {
public:
  DirectoryCertLoader(std::string certDir, std::string defaultCertIdentity);
  folly::Expected<LoadedCerts, std::string> load() const override;

  folly::Expected<std::string, std::string> getKeyPath() const override {
    return folly::makeUnexpected("Directory cert loader cannot provide single key file.");
  }

  folly::Expected<std::string, std::string> getCertPath() const override {
    return folly::makeUnexpected("Directory cert loader cannot provide single cert file.");
  }

private:
  std::string certDir_;
  std::string defaultCertIdentity_;
};

} // namespace openmoq::moqx::tls
