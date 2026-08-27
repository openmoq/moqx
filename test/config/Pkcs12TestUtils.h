/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <stdexcept>
#include <string>

#include <openssl/bio.h>
#include <openssl/pkcs12.h>

#include "../tls/CertTestUtils.h"
#include "../util/TempDir.h"

namespace openmoq::moqx::config::test {

using moqx::test::TempFile;

// Build a self-signed RSA cert + key and pack them into a PKCS#12 bundle,
// returning the DER bytes. `password` may be empty (password-less bundle).
// Throws std::runtime_error on any OpenSSL failure.
inline std::string makeSelfSignedPkcs12Der(const std::string& password) {
  auto [pkey, x509] =
      moqx::test::makeSelfSignedCert("moqx-test", {}, moqx::test::TestKeyType::RSA2048);

  // nid_key/nid_cert 0 => OpenSSL defaults; iter/mac_iter 0 => defaults.
  PKCS12* p12 =
      PKCS12_create(password.c_str(), "moqx-test", pkey, x509, /*ca=*/nullptr, 0, 0, 0, 0, 0);

  std::string der;
  if (p12) {
    BIO* bio = BIO_new(BIO_s_mem());
    if (bio && i2d_PKCS12_bio(bio, p12) == 1) {
      char* data = nullptr;
      long len = BIO_get_mem_data(bio, &data);
      if (len > 0 && data) {
        der.assign(data, static_cast<size_t>(len));
      }
    }
    if (bio) {
      BIO_free(bio);
    }
    PKCS12_free(p12);
  }

  X509_free(x509);
  EVP_PKEY_free(pkey);
  if (der.empty()) {
    throw std::runtime_error("failed to build PKCS#12 DER");
  }
  return der;
}

} // namespace openmoq::moqx::config::test
