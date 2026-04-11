/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <string>

#include <fizz/backend/openssl/certificate/CertUtils.h>
#include <fizz/protocol/Certificate.h>
#include <folly/Expected.h>
#include <folly/FileUtil.h>

namespace openmoq::moqx::tls {

/// Read a PEM cert+key pair from disk and return a fizz SelfCert.
inline folly::Expected<std::shared_ptr<fizz::SelfCert>, std::string>
loadCertKeyPair(const std::string& certPath, const std::string& keyPath) {
  std::string certData;
  if (!folly::readFile(certPath.c_str(), certData)) {
    return folly::makeUnexpected("Failed to read certificate file: " + certPath);
  }

  std::string keyData;
  if (!folly::readFile(keyPath.c_str(), keyData)) {
    return folly::makeUnexpected("Failed to read key file: " + keyPath);
  }

  try {
    return fizz::openssl::CertUtils::makeSelfCert(certData, keyData);
  } catch (const std::exception& e) {
    return folly::makeUnexpected(
        "Failed to parse certificate " + certPath + " / " + keyPath + ": " + e.what()
    );
  }
}

} // namespace openmoq::moqx::tls
