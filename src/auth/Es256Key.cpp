/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "auth/Es256Key.h"

#include <catapult/crypto.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/core_names.h>
#else
#include <openssl/ec.h>
#endif

#include <array>
#include <stdexcept>
#include <vector>

namespace openmoq::moqx::auth {
namespace {

using KeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

bool isP256(EVP_PKEY* key) {
  if (!key || EVP_PKEY_base_id(key) != EVP_PKEY_EC) {
    return false;
  }
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
  std::array<char, 80> group{};
  size_t groupLength = 0;
  return EVP_PKEY_get_utf8_string_param(
             key,
             OSSL_PKEY_PARAM_GROUP_NAME,
             group.data(),
             group.size(),
             &groupLength
         ) == 1 &&
         OBJ_txt2nid(group.data()) == NID_X9_62_prime256v1;
#else
  const auto* ec = EVP_PKEY_get0_EC_KEY(key);
  return ec && EC_GROUP_get_curve_name(EC_KEY_get0_group(ec)) == NID_X9_62_prime256v1;
#endif
}

KeyPtr readKey(const std::string& path, bool privateKey) {
  std::unique_ptr<BIO, decltype(&BIO_free)> input(BIO_new_file(path.c_str(), "rb"), BIO_free);
  if (!input) {
    throw std::runtime_error("cannot open ES256 key file: " + path);
  }
  // Refuse encrypted private keys instead of prompting the operator from a service/CLI.
  auto noPassword = [](char*, int, int, void*) { return 0; };
  KeyPtr key(
      privateKey ? PEM_read_bio_PrivateKey(input.get(), nullptr, noPassword, nullptr)
                 : PEM_read_bio_PUBKEY(input.get(), nullptr, nullptr, nullptr),
      EVP_PKEY_free
  );
  if (!isP256(key.get())) {
    throw std::invalid_argument(
        "ES256 requires a PEM P-256 " + std::string(privateKey ? "private" : "public") +
        " key: " + path
    );
  }
  return key;
}

std::vector<uint8_t> publicDer(EVP_PKEY* key) {
  const int size = i2d_PUBKEY(key, nullptr);
  if (size <= 0) {
    throw std::runtime_error("cannot encode ES256 public key");
  }
  std::vector<uint8_t> der(static_cast<size_t>(size));
  auto* output = der.data();
  if (i2d_PUBKEY(key, &output) != size) {
    throw std::runtime_error("cannot encode ES256 public key");
  }
  return der;
}

} // namespace

std::shared_ptr<const catapult::CryptographicAlgorithm>
loadEs256PublicKeyFile(const std::string& path) {
  auto key = readKey(path, false);
  return std::make_shared<catapult::Es256Algorithm>(publicDer(key.get()));
}

std::shared_ptr<const catapult::CryptographicAlgorithm>
loadEs256PrivateKeyFile(const std::string& path) {
  auto key = readKey(path, true);
  const int size = i2d_PrivateKey(key.get(), nullptr);
  if (size <= 0) {
    throw std::runtime_error("cannot encode ES256 private key");
  }
  catapult::SecureVector<uint8_t> der(static_cast<size_t>(size));
  auto* output = der.data();
  if (i2d_PrivateKey(key.get(), &output) != size) {
    throw std::runtime_error("cannot encode ES256 private key");
  }
  return std::make_shared<catapult::Es256Algorithm>(der, publicDer(key.get()));
}

} // namespace openmoq::moqx::auth
