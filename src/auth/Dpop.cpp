/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "auth/Dpop.h"

#include "auth/C4m.h"
#include "auth/CborReader.h"

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/pem.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <unordered_set>

namespace openmoq::moqx::auth {
namespace {

constexpr std::size_t kMaxProofBytes = 8192;
constexpr std::size_t kCoordinateBytes = 32;
constexpr std::string_view kProofType = "dpop-proof+cwt";

[[noreturn]] void reject(const char* why) {
  throw C4mError(AuthError::Forbidden, why);
}

void require(bool condition, const char* why = "malformed DPoP proof") {
  if (!condition) {
    throw C4mError(AuthError::Malformed, why);
  }
}

void appendHead(std::string& out, uint8_t major, uint64_t value) {
  const auto prefix = static_cast<uint8_t>(major << 5);
  if (value < 24) {
    out.push_back(static_cast<char>(prefix | value));
    return;
  }
  const unsigned width = value <= 0xff ? 1 : value <= 0xffff ? 2 : value <= 0xffffffff ? 4 : 8;
  const unsigned additional = width == 1 ? 24 : width == 2 ? 25 : width == 4 ? 26 : 27;
  out.push_back(static_cast<char>(prefix | additional));
  for (unsigned i = width; i > 0; --i) {
    out.push_back(static_cast<char>(value >> ((i - 1) * 8)));
  }
}

void appendInt(std::string& out, int64_t value) {
  appendHead(
      out,
      value < 0 ? 1 : 0,
      value < 0 ? static_cast<uint64_t>(-(value + 1)) : static_cast<uint64_t>(value)
  );
}

void appendString(std::string& out, uint8_t major, std::string_view bytes) {
  appendHead(out, major, bytes.size());
  out.append(bytes);
}

std::string base64Url(std::string_view data) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  std::size_t i = 0;
  for (; i + 2 < data.size(); i += 3) {
    const uint32_t v = (static_cast<uint8_t>(data[i]) << 16) |
                       (static_cast<uint8_t>(data[i + 1]) << 8) | static_cast<uint8_t>(data[i + 2]);
    out +=
        {kAlphabet[v >> 18], kAlphabet[(v >> 12) & 63], kAlphabet[(v >> 6) & 63], kAlphabet[v & 63]
        };
  }
  if (i + 1 == data.size()) {
    const uint32_t v = static_cast<uint8_t>(data[i]) << 16;
    out += {kAlphabet[v >> 18], kAlphabet[(v >> 12) & 63]};
  } else if (i + 2 == data.size()) {
    const uint32_t v =
        (static_cast<uint8_t>(data[i]) << 16) | (static_cast<uint8_t>(data[i + 1]) << 8);
    out += {kAlphabet[v >> 18], kAlphabet[(v >> 12) & 63], kAlphabet[(v >> 6) & 63]};
  }
  return out;
}

using PkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

PkeyPtr publicKeyFromCoordinates(std::string_view x, std::string_view y) {
  if (x.size() != kCoordinateBytes || y.size() != kCoordinateBytes) {
    reject("DPoP key is not a P-256 key");
  }
  std::string point(1, '\x04');
  point.append(x).append(y);
  std::unique_ptr<OSSL_PARAM_BLD, decltype(&OSSL_PARAM_BLD_free)> builder(
      OSSL_PARAM_BLD_new(),
      OSSL_PARAM_BLD_free
  );
  if (!builder ||
      OSSL_PARAM_BLD_push_utf8_string(builder.get(), OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0) !=
          1 ||
      OSSL_PARAM_BLD_push_octet_string(
          builder.get(),
          OSSL_PKEY_PARAM_PUB_KEY,
          point.data(),
          point.size()
      ) != 1) {
    throw std::runtime_error("cannot build DPoP key parameters");
  }
  std::unique_ptr<OSSL_PARAM, decltype(&OSSL_PARAM_free)> params(
      OSSL_PARAM_BLD_to_param(builder.get()),
      OSSL_PARAM_free
  );
  std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(
      EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr),
      EVP_PKEY_CTX_free
  );
  EVP_PKEY* raw = nullptr;
  if (!params || !ctx || EVP_PKEY_fromdata_init(ctx.get()) != 1 ||
      EVP_PKEY_fromdata(ctx.get(), &raw, EVP_PKEY_PUBLIC_KEY, params.get()) != 1) {
    reject("DPoP key is not a valid P-256 point");
  }
  return PkeyPtr(raw, EVP_PKEY_free);
}

std::string signatureInput(std::string_view protectedHeader, std::string_view payload) {
  std::string out(1, '\x84');
  appendString(out, 3, "Signature1");
  appendString(out, 2, protectedHeader);
  appendString(out, 2, {}); // external_aad
  appendString(out, 2, payload);
  return out;
}

bool verifyEs256(EVP_PKEY* key, std::string_view message, std::string_view raw) {
  if (raw.size() != 2 * kCoordinateBytes) {
    return false;
  }
  using BnPtr = std::unique_ptr<BIGNUM, decltype(&BN_free)>;
  const auto* bytes = reinterpret_cast<const unsigned char*>(raw.data());
  BnPtr r(BN_bin2bn(bytes, kCoordinateBytes, nullptr), BN_free);
  BnPtr s(BN_bin2bn(bytes + kCoordinateBytes, kCoordinateBytes, nullptr), BN_free);
  std::unique_ptr<ECDSA_SIG, decltype(&ECDSA_SIG_free)> sig(ECDSA_SIG_new(), ECDSA_SIG_free);
  if (!r || !s || !sig || ECDSA_SIG_set0(sig.get(), r.get(), s.get()) != 1) {
    return false;
  }
  r.release();
  s.release();
  unsigned char* der = nullptr;
  const int derLength = i2d_ECDSA_SIG(sig.get(), &der);
  if (derLength <= 0) {
    return false;
  }
  std::unique_ptr<unsigned char, decltype(&free)> derOwner(der, free);
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  return ctx && EVP_DigestVerifyInit(ctx.get(), nullptr, EVP_sha256(), nullptr, key) == 1 &&
         EVP_DigestVerify(
             ctx.get(),
             der,
             static_cast<std::size_t>(derLength),
             reinterpret_cast<const unsigned char*>(message.data()),
             message.size()
         ) == 1;
}

struct CoseKey {
  std::string x;
  std::string y;
};

CoseKey readCoseKey(CborReader& reader) {
  uint64_t count = 0;
  require(reader.readMapLen(count) && count == 4);
  CoseKey key;
  std::unordered_set<int64_t> seen;
  for (uint64_t i = 0; i < count; ++i) {
    int64_t label = 0;
    require(reader.readInt(label) && seen.insert(label).second);
    if (label == 1 || label == -1) {
      int64_t value = 0;
      require(reader.readInt(value));
      require(value == (label == 1 ? 2 : 1), "DPoP key is not EC2 P-256");
    } else if (label == -2 || label == -3) {
      std::string_view coordinate;
      require(reader.readByteString(coordinate) && coordinate.size() == kCoordinateBytes);
      (label == -2 ? key.x : key.y) = std::string(coordinate);
    } else {
      require(false);
    }
  }
  return key;
}

// kccs (14): CWT claims set {8 /cnf/: {1 /COSE_Key/: key}}.
CoseKey readKccs(CborReader& reader) {
  uint64_t count = 0;
  require(reader.readMapLen(count) && count == 1);
  int64_t label = 0;
  require(reader.readInt(label) && label == 8);
  require(reader.readMapLen(count) && count == 1);
  require(reader.readInt(label) && label == 1);
  return readCoseKey(reader);
}

struct ProofClaims {
  std::chrono::system_clock::time_point issuedAt;
  std::string cti;
  std::string athHash;
  std::string actionType;
  uint64_t action{0};
  std::vector<std::string> trackNamespace;
  std::optional<std::string> trackName;
};

ProofClaims readProofClaims(std::string_view payload) {
  CborReader reader(payload);
  uint64_t count = 0;
  require(reader.readMapLen(count) && count == 4);
  ProofClaims claims;
  std::unordered_set<int64_t> seen;
  for (uint64_t i = 0; i < count; ++i) {
    int64_t label = 0;
    require(reader.readInt(label) && seen.insert(label).second);
    if (label == 6) {
      int64_t seconds = 0;
      require(reader.readInt(seconds));
      claims.issuedAt = std::chrono::system_clock::time_point(std::chrono::seconds(seconds));
    } else if (label == 7) {
      std::string_view cti;
      require(reader.readByteString(cti) && !cti.empty() && cti.size() <= 64);
      claims.cti = std::string(cti);
    } else if (label == kDpopAthClaim) {
      std::string_view hash;
      require(reader.readByteString(hash));
      claims.athHash = std::string(hash);
    } else if (label == kDpopActxClaim) {
      uint64_t fields = 0;
      require(reader.readMapLen(fields) && fields >= 2 && fields <= 4);
      std::unordered_set<int64_t> actxSeen;
      for (uint64_t j = 0; j < fields; ++j) {
        int64_t key = 0;
        require(reader.readInt(key) && actxSeen.insert(key).second);
        if (key == 0) {
          std::string_view type;
          require(reader.readTextString(type));
          claims.actionType = std::string(type);
        } else if (key == 1) {
          require(reader.readUInt(claims.action));
        } else if (key == 2) {
          uint64_t n = 0;
          require(reader.readArrayLen(n) && n <= 32 && n <= reader.remaining());
          for (uint64_t k = 0; k < n; ++k) {
            std::string_view field;
            require(reader.readByteString(field));
            claims.trackNamespace.emplace_back(field);
          }
        } else if (key == 3) {
          std::string_view track;
          require(reader.readByteString(track));
          claims.trackName = std::string(track);
        } else {
          require(false);
        }
      }
      require(actxSeen.contains(0) && actxSeen.contains(1));
    } else {
      require(false);
    }
  }
  require(
      reader.eof() && seen.contains(6) && seen.contains(7) && seen.contains(kDpopAthClaim) &&
      seen.contains(kDpopActxClaim)
  );
  return claims;
}

} // namespace

std::string sha256(std::string_view data) {
  std::string out(SHA256_DIGEST_LENGTH, '\0');
  SHA256(
      reinterpret_cast<const unsigned char*>(data.data()),
      data.size(),
      reinterpret_cast<unsigned char*>(out.data())
  );
  return out;
}

std::string coseKeyThumbprint(std::string_view x, std::string_view y) {
  // RFC 9679: deterministic CBOR of the required EC2 members, in bytewise key order.
  std::string canonical;
  appendHead(canonical, 5, 4);
  appendInt(canonical, 1);
  appendInt(canonical, 2);
  appendInt(canonical, -1);
  appendInt(canonical, 1);
  appendInt(canonical, -2);
  appendString(canonical, 2, x);
  appendInt(canonical, -3);
  appendString(canonical, 2, y);
  return sha256(canonical);
}

std::string jwkThumbprint(std::string_view x, std::string_view y) {
  // RFC 7638: required members in lexicographic order, no whitespace.
  return sha256(
      R"({"crv":"P-256","kty":"EC","x":")" + base64Url(x) + R"(","y":")" + base64Url(y) + R"("})"
  );
}

VerifiedDpopProof verifyDpopProof(
    std::string_view proof,
    std::string_view tokenHash,
    const DpopRequirement& requirement,
    const DpopContext& context,
    std::chrono::system_clock::time_point now
) {
  require(!proof.empty() && proof.size() <= kMaxProofBytes);
  CborReader reader(proof);
  if (reader.peekMajor() == 6) {
    uint64_t tag = 0;
    require(reader.readTag(tag) && tag == 18);
  }
  uint64_t length = 0;
  std::string_view protectedHeader;
  require(reader.readArrayLen(length) && length == 4 && reader.readByteString(protectedHeader));
  uint64_t unprotectedCount = 0;
  require(reader.readMapLen(unprotectedCount) && unprotectedCount == 0);
  std::string_view payload;
  std::string_view signature;
  require(reader.readByteString(payload) && reader.readByteString(signature) && reader.eof());

  // Protected header: alg, typ, kccs.
  CborReader header(protectedHeader);
  uint64_t entries = 0;
  require(header.readMapLen(entries) && entries == 3);
  std::unordered_set<int64_t> seen;
  std::optional<CoseKey> key;
  for (uint64_t i = 0; i < entries; ++i) {
    int64_t label = 0;
    require(header.readInt(label) && seen.insert(label).second);
    if (label == 1) {
      int64_t alg = 0;
      require(header.readInt(alg));
      if (alg != -7) {
        reject("unsupported DPoP proof algorithm"); // Only ES256; symmetric algs are forbidden.
      }
    } else if (label == 16) {
      std::string_view type;
      require(header.readTextString(type));
      if (type != kProofType) {
        reject("DPoP proof has the wrong typ");
      }
    } else if (label == 14) {
      key = readKccs(header);
    } else {
      require(false);
    }
  }
  require(header.eof() && seen.contains(1) && seen.contains(16) && key.has_value());

  // The proof's key must match the token's cnf before it is trusted to sign anything.
  const auto thumbprint = requirement.binding.kind == DpopBinding::Kind::Ckt
                              ? coseKeyThumbprint(key->x, key->y)
                              : jwkThumbprint(key->x, key->y);
  if (thumbprint != requirement.binding.thumbprint) {
    reject("DPoP key does not match token binding");
  }
  const auto publicKey = publicKeyFromCoordinates(key->x, key->y);
  if (!verifyEs256(publicKey.get(), signatureInput(protectedHeader, payload), signature)) {
    reject("invalid DPoP proof signature");
  }

  const auto claims = readProofClaims(payload);
  if (claims.athHash != tokenHash) {
    reject("DPoP proof is bound to a different token");
  }
  const auto skew = claims.issuedAt > now ? claims.issuedAt - now : now - claims.issuedAt;
  if (skew > requirement.window) {
    reject("DPoP proof is outside the freshness window");
  }
  if (claims.actionType != "moqt" || claims.action != static_cast<uint64_t>(context.action) ||
      claims.trackNamespace != context.trackNamespace || claims.trackName != context.trackName) {
    reject("DPoP proof does not match the requested action");
  }
  return VerifiedDpopProof{claims.cti, claims.issuedAt};
}

std::pair<std::string, std::string> es256PublicCoordinates(const std::string& pemPath) {
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new_file(pemPath.c_str(), "r"), BIO_free);
  if (!bio) {
    throw std::invalid_argument("cannot read key file: " + pemPath);
  }
  const auto noPassword = [](char*, int, int, void*) { return 0; };
  PkeyPtr key(PEM_read_bio_PrivateKey(bio.get(), nullptr, noPassword, nullptr), EVP_PKEY_free);
  if (!key) {
    BIO_seek(bio.get(), 0);
    key.reset(PEM_read_bio_PUBKEY(bio.get(), nullptr, noPassword, nullptr));
  }
  unsigned char point[65];
  std::size_t length = 0;
  if (!key ||
      EVP_PKEY_get_octet_string_param(
          key.get(),
          OSSL_PKEY_PARAM_PUB_KEY,
          point,
          sizeof(point),
          &length
      ) != 1 ||
      length != sizeof(point) || point[0] != 0x04) {
    throw std::invalid_argument("not an unencrypted PEM P-256 key: " + pemPath);
  }
  return {
      std::string(reinterpret_cast<char*>(point) + 1, kCoordinateBytes),
      std::string(reinterpret_cast<char*>(point) + 1 + kCoordinateBytes, kCoordinateBytes)
  };
}

std::string signDpopProof(
    const std::string& privateKeyPemPath,
    std::string_view token,
    const DpopContext& context,
    std::string_view cti,
    std::chrono::system_clock::time_point issuedAt
) {
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(
      BIO_new_file(privateKeyPemPath.c_str(), "r"),
      BIO_free
  );
  const auto noPassword = [](char*, int, int, void*) { return 0; };
  PkeyPtr key(
      bio ? PEM_read_bio_PrivateKey(bio.get(), nullptr, noPassword, nullptr) : nullptr,
      EVP_PKEY_free
  );
  if (!key) {
    throw std::invalid_argument("cannot load unencrypted PEM private key: " + privateKeyPemPath);
  }
  const auto [x, y] = es256PublicCoordinates(privateKeyPemPath);

  std::string protectedHeader;
  appendHead(protectedHeader, 5, 3);
  appendInt(protectedHeader, 1);
  appendInt(protectedHeader, -7);
  appendInt(protectedHeader, 16);
  appendString(protectedHeader, 3, kProofType);
  appendInt(protectedHeader, 14);
  appendHead(protectedHeader, 5, 1);
  appendInt(protectedHeader, 8);
  appendHead(protectedHeader, 5, 1);
  appendInt(protectedHeader, 1);
  appendHead(protectedHeader, 5, 4);
  appendInt(protectedHeader, 1);
  appendInt(protectedHeader, 2);
  appendInt(protectedHeader, -1);
  appendInt(protectedHeader, 1);
  appendInt(protectedHeader, -2);
  appendString(protectedHeader, 2, x);
  appendInt(protectedHeader, -3);
  appendString(protectedHeader, 2, y);

  std::string payload;
  appendHead(payload, 5, 4);
  appendInt(payload, 6);
  appendInt(
      payload,
      std::chrono::duration_cast<std::chrono::seconds>(issuedAt.time_since_epoch()).count()
  );
  appendInt(payload, 7);
  appendString(payload, 2, cti);
  appendInt(payload, kDpopActxClaim);
  appendHead(
      payload,
      5,
      2 + (context.trackNamespace.empty() ? 0 : 1) + (context.trackName ? 1 : 0)
  );
  appendInt(payload, 0);
  appendString(payload, 3, "moqt");
  appendInt(payload, 1);
  appendInt(payload, static_cast<int64_t>(context.action));
  if (!context.trackNamespace.empty()) {
    appendInt(payload, 2);
    appendHead(payload, 4, context.trackNamespace.size());
    for (const auto& field : context.trackNamespace) {
      appendString(payload, 2, field);
    }
  }
  if (context.trackName) {
    appendInt(payload, 3);
    appendString(payload, 2, *context.trackName);
  }
  appendInt(payload, kDpopAthClaim);
  appendString(payload, 2, sha256(token));

  const auto message = signatureInput(protectedHeader, payload);
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  std::size_t derLength = 0;
  std::string der;
  if (!ctx || EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr, key.get()) != 1 ||
      EVP_DigestSign(
          ctx.get(),
          nullptr,
          &derLength,
          reinterpret_cast<const unsigned char*>(message.data()),
          message.size()
      ) != 1) {
    throw std::runtime_error("cannot sign DPoP proof");
  }
  der.resize(derLength);
  if (EVP_DigestSign(
          ctx.get(),
          reinterpret_cast<unsigned char*>(der.data()),
          &derLength,
          reinterpret_cast<const unsigned char*>(message.data()),
          message.size()
      ) != 1) {
    throw std::runtime_error("cannot sign DPoP proof");
  }
  const auto* derBytes = reinterpret_cast<const unsigned char*>(der.data());
  std::unique_ptr<ECDSA_SIG, decltype(&ECDSA_SIG_free)> sig(
      d2i_ECDSA_SIG(nullptr, &derBytes, static_cast<long>(derLength)),
      ECDSA_SIG_free
  );
  const BIGNUM* r = nullptr;
  const BIGNUM* s = nullptr;
  if (!sig) {
    throw std::runtime_error("cannot parse DPoP signature");
  }
  ECDSA_SIG_get0(sig.get(), &r, &s);
  std::string raw(2 * kCoordinateBytes, '\0');
  auto* rawBytes = reinterpret_cast<unsigned char*>(raw.data());
  if (BN_bn2binpad(r, rawBytes, kCoordinateBytes) != static_cast<int>(kCoordinateBytes) ||
      BN_bn2binpad(s, rawBytes + kCoordinateBytes, kCoordinateBytes) !=
          static_cast<int>(kCoordinateBytes)) {
    throw std::runtime_error("cannot encode DPoP signature");
  }

  std::string out;
  appendHead(out, 6, 18);
  appendHead(out, 4, 4);
  appendString(out, 2, protectedHeader);
  appendHead(out, 5, 0);
  appendString(out, 2, payload);
  appendString(out, 2, raw);
  return out;
}

} // namespace openmoq::moqx::auth
