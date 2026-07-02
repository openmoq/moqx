/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pkcs12.h>
#include <openssl/x509.h>

namespace openmoq::moqx::config::test {

// Build a self-signed RSA cert + key and pack them into a PKCS#12 bundle,
// returning the DER bytes. `password` may be empty (password-less bundle).
// Throws std::runtime_error on any OpenSSL failure. Test-only.
inline std::string makeSelfSignedPkcs12Der(const std::string& password) {
  EVP_PKEY* pkey = EVP_RSA_gen(2048);
  if (!pkey) {
    throw std::runtime_error("EVP_RSA_gen failed");
  }
  X509* x509 = X509_new();
  if (!x509) {
    EVP_PKEY_free(pkey);
    throw std::runtime_error("X509_new failed");
  }
  ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
  X509_gmtime_adj(X509_getm_notBefore(x509), 0);
  X509_gmtime_adj(X509_getm_notAfter(x509), 3600);
  X509_set_pubkey(x509, pkey);
  X509_NAME* name = X509_get_subject_name(x509);
  X509_NAME_add_entry_by_txt(
      name,
      "CN",
      MBSTRING_ASC,
      reinterpret_cast<const unsigned char*>("moqx-test"),
      -1,
      -1,
      0
  );
  X509_set_issuer_name(x509, name); // self-signed
  bool ok = X509_sign(x509, pkey, EVP_sha256()) > 0;

  PKCS12* p12 = nullptr;
  if (ok) {
    // nid_key/nid_cert 0 => OpenSSL defaults; iter/mac_iter 0 => defaults.
    p12 = PKCS12_create(password.c_str(), "moqx-test", pkey, x509, /*ca=*/nullptr, 0, 0, 0, 0, 0);
  }

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

// RAII temp file: writes the given bytes (binary) to a unique path, removes it
// on destruction. Test-only.
class TempFile {
public:
  TempFile(std::string_view bytes, std::string_view suffix) {
    static std::atomic<int> counter{0};
    path_ =
        std::filesystem::temp_directory_path() / ("moqx_test_" + std::to_string(::getpid()) + "_" +
                                                  std::to_string(counter++) + std::string(suffix));
    std::ofstream ofs(path_, std::ios::binary);
    ofs.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  ~TempFile() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;

  std::string path() const { return path_.string(); }

private:
  std::filesystem::path path_;
};

} // namespace openmoq::moqx::config::test
