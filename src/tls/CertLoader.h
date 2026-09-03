/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <stdexcept>
#include <string>

#include <fizz/backend/openssl/certificate/CertUtils.h>
#include <fizz/util/Status.h>
#include <folly/FileUtil.h>

namespace openmoq::moqx::tls {

// Build a fizz SelfCert from PEM buffers; `what` names the source in the
// error message. Throws std::runtime_error on failure.
inline std::unique_ptr<fizz::SelfCert> makeSelfCertFromPems(
    const std::string& certPem,
    const std::string& keyPem,
    const std::string& what
) {
  std::unique_ptr<fizz::SelfCert> cert;
  fizz::Error err;
  if (fizz::openssl::CertUtils::makeSelfCert(cert, err, certPem, keyPem) != fizz::Status::Success) {
    throw std::runtime_error(
        "failed to load TLS certificate/key " + what + ": " +
        (err.msg() ? err.msg() : "unknown error")
    );
  }
  return cert;
}

// Load a SelfCert from a cert/key file pair. Throws std::runtime_error naming
// the offending path.
inline std::shared_ptr<fizz::SelfCert>
loadCertPair(const std::string& certPath, const std::string& keyPath) {
  std::string certPem;
  std::string keyPem;
  if (!folly::readFile(certPath.c_str(), certPem)) {
    throw std::runtime_error(
        "failed to read TLS certificate '" + certPath +
        "' - check the path exists and is readable by this process"
    );
  }
  if (!folly::readFile(keyPath.c_str(), keyPem)) {
    throw std::runtime_error(
        "failed to read TLS key '" + keyPath +
        "' - check the path exists and is readable by this process"
    );
  }
  return makeSelfCertFromPems(certPem, keyPem, "(cert='" + certPath + "', key='" + keyPath + "')");
}

} // namespace openmoq::moqx::tls
