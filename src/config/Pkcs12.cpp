/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "config/Pkcs12.h"

#include <memory>

#include <folly/FileUtil.h>
#include <folly/ssl/OpenSSLCertUtils.h>
#include <folly/ssl/OpenSSLPtrTypes.h>

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/x509.h>

namespace openmoq::moqx::config {

namespace {

// folly/ssl provides RAII wrappers for BIO, EVP_PKEY, X509, and STACK_OF(X509);
// only PKCS12 lacks one, so define that single deleter here.
struct Pkcs12Deleter {
  void operator()(PKCS12* p) const noexcept {
    if (p) {
      PKCS12_free(p);
    }
  }
};
using Pkcs12Ptr = std::unique_ptr<PKCS12, Pkcs12Deleter>;

folly::Unexpected<std::string> err(const std::string& msg) {
  return folly::makeUnexpected(msg);
}

} // namespace

folly::Expected<TlsMaterial, std::string>
transcodePkcs12(folly::StringPiece der, folly::StringPiece password) {
  if (der.empty()) {
    return err("PKCS#12 data is empty");
  }

  folly::ssl::BioUniquePtr bio(BIO_new_mem_buf(der.data(), static_cast<int>(der.size())));
  if (!bio) {
    return err("failed to allocate BIO for PKCS#12 data");
  }

  Pkcs12Ptr p12(d2i_PKCS12_bio(bio.get(), nullptr));
  if (!p12) {
    return err("failed to parse PKCS#12 structure (not a valid .p12/.pfx file)");
  }

  // PKCS12_parse needs a NUL-terminated password; StringPiece is not guaranteed
  // NUL-terminated, so copy into a local string and wipe it after parsing.
  std::string pass(password.data(), password.size());
  EVP_PKEY* pkeyRaw = nullptr;
  X509* leafRaw = nullptr;
  STACK_OF(X509)* chainRaw = nullptr;
  int rc = PKCS12_parse(p12.get(), pass.c_str(), &pkeyRaw, &leafRaw, &chainRaw);
  if (!pass.empty()) {
    OPENSSL_cleanse(pass.data(), pass.size());
  }

  folly::ssl::EvpPkeyUniquePtr pkey(pkeyRaw);
  folly::ssl::X509UniquePtr leaf(leafRaw);
  folly::ssl::OwningStackOfX509UniquePtr chain(chainRaw);

  if (rc != 1) {
    return err("failed to decrypt/parse PKCS#12 bundle (wrong password or corrupt data)");
  }
  if (!pkey) {
    return err("PKCS#12 bundle contains no private key");
  }
  if (!leaf) {
    return err("PKCS#12 bundle contains no certificate");
  }

  // Certificate chain PEM: leaf first, then any intermediates. pemEncode throws
  // on failure; convert to our error-string contract so config load stays clean.
  std::string certChainPem;
  try {
    certChainPem = folly::ssl::OpenSSLCertUtils::pemEncode(*leaf);
    const int n = chain ? sk_X509_num(chain.get()) : 0;
    for (int i = 0; i < n; ++i) {
      certChainPem += folly::ssl::OpenSSLCertUtils::pemEncode(*sk_X509_value(chain.get(), i));
    }
  } catch (const std::exception& e) {
    return err(std::string("failed to serialize certificate to PEM: ") + e.what());
  }

  // Private key PEM, unencrypted (no cipher). folly's cert utils are
  // certificate-only, so serialize the key directly.
  folly::ssl::BioUniquePtr kbio(BIO_new(BIO_s_mem()));
  if (!kbio) {
    return err("failed to allocate BIO for private key PEM");
  }
  if (PEM_write_bio_PrivateKey(kbio.get(), pkey.get(), nullptr, nullptr, 0, nullptr, nullptr) !=
      1) {
    return err("failed to serialize private key to PEM");
  }
  char* keyData = nullptr;
  long keyLen = BIO_get_mem_data(kbio.get(), &keyData);
  if (keyLen <= 0 || keyData == nullptr) {
    return err("failed to serialize private key to PEM");
  }
  std::string keyPem(keyData, static_cast<size_t>(keyLen));

  return TlsMaterial{.certChainPem = std::move(certChainPem), .keyPem = std::move(keyPem)};
}

folly::Expected<TlsMaterial, std::string>
loadPkcs12File(const std::string& path, folly::StringPiece password) {
  std::string der;
  if (!folly::readFile(path.c_str(), der)) {
    return err("failed to read PKCS#12 file '" + path + "'");
  }
  return transcodePkcs12(der, password);
}

} // namespace openmoq::moqx::config
