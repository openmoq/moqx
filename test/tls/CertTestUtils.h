/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "../util/TempDir.h"

namespace openmoq::moqx::test {

// EC256 is the default: keygen is orders of magnitude faster than RSA.
enum class TestKeyType { EC256, RSA2048 };

// Self-signed cert + key as raw OpenSSL handles; the caller owns both frees.
// `sanDns` entries become DNS SANs (leading wildcard label allowed); empty
// means no SAN extension. An empty `cn` omits the CN attribute entirely.
// Throws std::runtime_error on failure.
inline std::pair<EVP_PKEY*, X509*> makeSelfSignedCert(
    const std::string& cn,
    const std::vector<std::string>& sanDns = {},
    TestKeyType keyType = TestKeyType::EC256
) {
  EVP_PKEY* pkey = keyType == TestKeyType::RSA2048 ? EVP_RSA_gen(2048) : EVP_EC_gen("P-256");
  if (!pkey) {
    throw std::runtime_error("key generation failed");
  }
  X509* x509 = X509_new();
  if (!x509) {
    EVP_PKEY_free(pkey);
    throw std::runtime_error("X509_new failed");
  }
  bool ok = true;
  ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
  X509_gmtime_adj(X509_getm_notBefore(x509), 0);
  X509_gmtime_adj(X509_getm_notAfter(x509), 3600);
  X509_set_pubkey(x509, pkey);
  X509_NAME* name = X509_get_subject_name(x509);
  if (!cn.empty()) {
    X509_NAME_add_entry_by_txt(
        name,
        "CN",
        MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>(cn.c_str()),
        -1,
        -1,
        0
    );
  } else {
    // A DN with no attributes at all makes some parsers unhappy; give the
    // CN-less cert an O attribute so the subject stays non-empty.
    X509_NAME_add_entry_by_txt(
        name,
        "O",
        MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("moqx-test-no-cn"),
        -1,
        -1,
        0
    );
  }
  X509_set_issuer_name(x509, name); // self-signed

  if (!sanDns.empty()) {
    std::string sanValue;
    for (const auto& dns : sanDns) {
      if (!sanValue.empty()) {
        sanValue += ",";
      }
      sanValue += "DNS:" + dns;
    }
    X509_EXTENSION* ext =
        X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name, sanValue.c_str());
    if (ext) {
      X509_add_ext(x509, ext, -1);
      X509_EXTENSION_free(ext);
    } else {
      ok = false;
    }
  }

  ok = ok && X509_sign(x509, pkey, EVP_sha256()) > 0;
  if (!ok) {
    X509_free(x509);
    EVP_PKEY_free(pkey);
    throw std::runtime_error("failed to build self-signed cert");
  }
  return {pkey, x509};
}

struct PemPair {
  std::string certPem;
  std::string keyPem;
};

// PEM-encoded self-signed cert + unencrypted private key.
inline PemPair makeSelfSignedCertPem(
    const std::string& cn,
    const std::vector<std::string>& sanDns = {},
    TestKeyType keyType = TestKeyType::EC256
) {
  auto [pkey, x509] = makeSelfSignedCert(cn, sanDns, keyType);

  PemPair out;
  bool ok = false;
  BIO* bio = BIO_new(BIO_s_mem());
  if (bio) {
    ok = PEM_write_bio_X509(bio, x509) == 1;
    if (ok) {
      char* data = nullptr;
      long len = BIO_get_mem_data(bio, &data);
      out.certPem.assign(data, static_cast<size_t>(len));
    }
    BIO_free(bio);
  }
  bio = ok ? BIO_new(BIO_s_mem()) : nullptr;
  ok = false;
  if (bio) {
    ok = PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr) == 1;
    if (ok) {
      char* data = nullptr;
      long len = BIO_get_mem_data(bio, &data);
      out.keyPem.assign(data, static_cast<size_t>(len));
    }
    BIO_free(bio);
  }

  X509_free(x509);
  EVP_PKEY_free(pkey);
  if (!ok) {
    throw std::runtime_error("failed to PEM-encode cert/key");
  }
  return out;
}

// Write a cert/key pair as <base>.pem + <base>.key into the directory.
inline void writePair(const TempDir& dir, const std::string& base, const PemPair& pair) {
  dir.writeFile(base + ".pem", pair.certPem);
  dir.writeFile(base + ".key", pair.keyPem);
}

} // namespace openmoq::moqx::test
