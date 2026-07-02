/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "auth/HmacKey.h"

#include <openssl/evp.h>
#include <openssl/kdf.h>

#include <memory>
#include <stdexcept>

namespace openmoq::moqx::auth {
namespace {

// Derive a 256-bit HMAC key from the configured secret using HKDF-SHA256
// (RFC 5869) rather than a bare hash. The fixed, non-secret salt and info
// strings provide domain separation so the same configured secret can't yield
// identical key material if reused for another purpose. (HKDF does not add
// entropy: a low-entropy secret is still weak -- operators should use a long,
// random secret.)
constexpr std::string_view kHkdfSalt = "moqx-catapult-v1";
constexpr std::string_view kHkdfInfo = "moqx-catapult-hmac-token-verify";

} // namespace

std::vector<uint8_t> deriveHmacKey(std::string_view secret) {
  std::vector<uint8_t> key(32); // 256-bit output for HMAC-SHA256

  auto* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
  if (!ctx) {
    throw std::runtime_error("EVP_PKEY_CTX_new_id(HKDF) failed");
  }
  auto guard = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>(ctx, EVP_PKEY_CTX_free);

  size_t keyLen = key.size();
  if (EVP_PKEY_derive_init(ctx) <= 0 || EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha256()) <= 0 ||
      EVP_PKEY_CTX_set1_hkdf_salt(
          ctx,
          reinterpret_cast<const unsigned char*>(kHkdfSalt.data()),
          static_cast<int>(kHkdfSalt.size())
      ) <= 0 ||
      EVP_PKEY_CTX_set1_hkdf_key(
          ctx,
          reinterpret_cast<const unsigned char*>(secret.data()),
          static_cast<int>(secret.size())
      ) <= 0 ||
      EVP_PKEY_CTX_add1_hkdf_info(
          ctx,
          reinterpret_cast<const unsigned char*>(kHkdfInfo.data()),
          static_cast<int>(kHkdfInfo.size())
      ) <= 0 ||
      EVP_PKEY_derive(ctx, key.data(), &keyLen) <= 0) {
    throw std::runtime_error("HKDF key derivation failed");
  }
  key.resize(keyLen);
  return key;
}

} // namespace openmoq::moqx::auth
