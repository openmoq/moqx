/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "config/Pkcs12.h"

#include <memory>
#include <optional>

#include <folly/FileUtil.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/x509.h>

namespace openmoq::moqx::config {

namespace {

// OpenSSL RAII deleters.
struct BioDeleter {
  void operator()(BIO* b) const noexcept {
    if (b) {
      BIO_free(b);
    }
  }
};
struct Pkcs12Deleter {
  void operator()(PKCS12* p) const noexcept {
    if (p) {
      PKCS12_free(p);
    }
  }
};
struct EvpPkeyDeleter {
  void operator()(EVP_PKEY* k) const noexcept {
    if (k) {
      EVP_PKEY_free(k);
    }
  }
};
struct X509Deleter {
  void operator()(X509* c) const noexcept {
    if (c) {
      X509_free(c);
    }
  }
};
struct X509StackDeleter {
  void operator()(STACK_OF(X509) * s) const noexcept {
    if (s) {
      sk_X509_pop_free(s, X509_free);
    }
  }
};

using BioPtr = std::unique_ptr<BIO, BioDeleter>;
using Pkcs12Ptr = std::unique_ptr<PKCS12, Pkcs12Deleter>;
using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>;
using X509Ptr = std::unique_ptr<X509, X509Deleter>;
using X509StackPtr = std::unique_ptr<STACK_OF(X509), X509StackDeleter>;

folly::Unexpected<std::string> err(const std::string& msg) {
  return folly::makeUnexpected(msg);
}

// Copy the contents of a memory BIO into a std::string.
std::string bioToString(BIO* bio) {
  char* data = nullptr;
  long len = BIO_get_mem_data(bio, &data);
  if (len <= 0 || data == nullptr) {
    return {};
  }
  return std::string(data, static_cast<size_t>(len));
}

// Append a single certificate as PEM to `out`. Returns an error string on
// serialization failure, std::nullopt on success.
std::optional<std::string> appendCertPem(std::string& out, X509* cert) {
  BioPtr bio(BIO_new(BIO_s_mem()));
  if (!bio) {
    return std::string("failed to allocate BIO for certificate PEM");
  }
  if (PEM_write_bio_X509(bio.get(), cert) != 1) {
    return std::string("failed to serialize certificate to PEM");
  }
  out += bioToString(bio.get());
  return std::nullopt;
}

} // namespace

folly::Expected<TlsMaterial, std::string>
transcodePkcs12(folly::StringPiece der, folly::StringPiece password) {
  if (der.empty()) {
    return err("PKCS#12 data is empty");
  }

  BioPtr bio(BIO_new_mem_buf(der.data(), static_cast<int>(der.size())));
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

  EvpPkeyPtr pkey(pkeyRaw);
  X509Ptr leaf(leafRaw);
  X509StackPtr chain(chainRaw);

  if (rc != 1) {
    return err("failed to decrypt/parse PKCS#12 bundle (wrong password or corrupt data)");
  }
  if (!pkey) {
    return err("PKCS#12 bundle contains no private key");
  }
  if (!leaf) {
    return err("PKCS#12 bundle contains no certificate");
  }

  // Certificate chain PEM: leaf first, then any intermediates.
  std::string certChainPem;
  if (auto e = appendCertPem(certChainPem, leaf.get())) {
    return folly::makeUnexpected(*e);
  }
  const int n = chain ? sk_X509_num(chain.get()) : 0;
  for (int i = 0; i < n; ++i) {
    if (auto e = appendCertPem(certChainPem, sk_X509_value(chain.get(), i))) {
      return folly::makeUnexpected(*e);
    }
  }

  // Private key PEM, unencrypted (no cipher).
  std::string keyPem;
  {
    BioPtr kbio(BIO_new(BIO_s_mem()));
    if (!kbio) {
      return err("failed to allocate BIO for private key PEM");
    }
    if (PEM_write_bio_PrivateKey(kbio.get(), pkey.get(), nullptr, nullptr, 0, nullptr, nullptr) !=
        1) {
      return err("failed to serialize private key to PEM");
    }
    keyPem = bioToString(kbio.get());
  }

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
