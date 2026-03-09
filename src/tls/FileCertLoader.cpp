/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "FileCertLoader.h"
#include "CertUtils.h"

namespace openmoq::moqx::tls {

FileCertLoader::FileCertLoader(std::string certFile, std::string keyFile)
    : certFile_(std::move(certFile)), keyFile_(std::move(keyFile)) {}

folly::Expected<LoadedCerts, std::string> FileCertLoader::load() const {
  auto cert = loadCertKeyPair(certFile_, keyFile_);
  if (cert.hasError()) {
    return folly::makeUnexpected(std::move(cert.error()));
  }

  auto identity = cert.value()->getIdentity();
  LoadedCerts result;
  result.defaultIdentity = identity;
  result.certs.push_back(LoadedCerts::Entry{
      .identity = std::move(identity),
      .cert = std::move(cert.value()),
  });
  return result;
}

} // namespace openmoq::moqx::tls
