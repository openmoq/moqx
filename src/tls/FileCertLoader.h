/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>

#include "TlsCertLoader.h"

namespace openmoq::moqx::tls {

class FileCertLoader : public TlsCertLoader {
public:
  FileCertLoader(std::string certFile, std::string keyFile);
  folly::Expected<LoadedCerts, std::string> load() const override;

  folly::Expected<std::string, std::string> getKeyPath() const override {
    return folly::makeExpected<std::string>(keyFile_);
  }

  folly::Expected<std::string, std::string> getCertPath() const override {
    return folly::makeExpected<std::string>(certFile_);
  };

private:
  std::string certFile_;
  std::string keyFile_;
};

} // namespace openmoq::moqx::tls
