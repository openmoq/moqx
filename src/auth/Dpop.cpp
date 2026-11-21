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
constexpr std::string_view kProofType = "dpop-proof+cwt";

// The proof algorithms and the key shape each requires (RFC 9053 / RFC 8037).
struct KeyShape {
  int64_t algorithm;
  int64_t kty;      // COSE key type: 2 = EC2, 1 = OKP
  int64_t crv;      // COSE curve: 1 = P-256, 2 = P-384, 6 = Ed25519
  std::size_t size; // coordinate size in bytes
  const char* jwkKty;
  const char* jwkCrv;
  const char* group; // OpenSSL group name for EC keys, null for Ed25519
};

constexpr KeyShape kShapes[] = {
    {kDpopAlgEs256, 2, 1, 32, "EC", "P-256", "prime256v1"},
    {kDpopAlgEs384, 2, 2, 48, "EC", "P-384", "secp384r1"},
    {kDpopAlgEdDsa, 1, 6, 32, "OKP", "Ed25519", nullptr},
};

const KeyShape* shapeForAlgorithm(int64_t algorithm) {
  for (const auto& shape : kShapes) {
    if (shape.algorithm == algorithm) {
      return &shape;
    }
  }
  return nullptr;
}

const KeyShape& requireShape(int64_t algorithm) {
  const auto* shape = shapeForAlgorithm(algorithm);
  if (shape == nullptr) {
    throw std::invalid_argument("unsupported DPoP key algorithm");
  }
  return *shape;
}

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

PkeyPtr publicKeyFromKey(const DpopKey& key) {
  const auto& shape = requireShape(key.algorithm);
  if (key.x.size() != shape.size || (shape.group != nullptr && key.y.size() != shape.size) ||
      (shape.group == nullptr && !key.y.empty())) {
    reject("DPoP key has the wrong size for its algorithm");
  }
  if (shape.group == nullptr) {
    PkeyPtr edKey(
        EVP_PKEY_new_raw_public_key(
            EVP_PKEY_ED25519,
            nullptr,
            reinterpret_cast<const unsigned char*>(key.x.data()),
            key.x.size()
        ),
        EVP_PKEY_free
    );
    if (!edKey) {
      reject("DPoP key is not a valid Ed25519 key");
    }
    return edKey;
  }
  std::string point(1, '\x04');
  point.append(key.x).append(key.y);
  std::unique_ptr<OSSL_PARAM_BLD, decltype(&OSSL_PARAM_BLD_free)> builder(
      OSSL_PARAM_BLD_new(),
      OSSL_PARAM_BLD_free
  );
  if (!builder ||
      OSSL_PARAM_BLD_push_utf8_string(builder.get(), OSSL_PKEY_PARAM_GROUP_NAME, shape.group, 0) !=
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
    reject("DPoP key is not a valid point on its curve");
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

const EVP_MD* digestFor(int64_t algorithm) {
  return algorithm == kDpopAlgEs384   ? EVP_sha384()
         : algorithm == kDpopAlgEs256 ? EVP_sha256()
                                      : nullptr;
}

bool verifyProofSignature(
    const DpopKey& key,
    EVP_PKEY* publicKey,
    std::string_view message,
    std::string_view raw
) {
  const auto& shape = requireShape(key.algorithm);
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!ctx) {
    return false;
  }
  const auto* message_ = reinterpret_cast<const unsigned char*>(message.data());
  if (shape.group == nullptr) {
    // Ed25519 signs the message directly (no digest) and the signature is already 64 raw bytes.
    return raw.size() == 64 &&
           EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, publicKey) == 1 &&
           EVP_DigestVerify(
               ctx.get(),
               reinterpret_cast<const unsigned char*>(raw.data()),
               raw.size(),
               message_,
               message.size()
           ) == 1;
  }
  if (raw.size() != 2 * shape.size) {
    return false;
  }
  using BnPtr = std::unique_ptr<BIGNUM, decltype(&BN_free)>;
  const auto* bytes = reinterpret_cast<const unsigned char*>(raw.data());
  BnPtr r(BN_bin2bn(bytes, static_cast<int>(shape.size), nullptr), BN_free);
  BnPtr s(BN_bin2bn(bytes + shape.size, static_cast<int>(shape.size), nullptr), BN_free);
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
  return EVP_DigestVerifyInit(ctx.get(), nullptr, digestFor(key.algorithm), nullptr, publicKey) ==
             1 &&
         EVP_DigestVerify(
             ctx.get(),
             der,
             static_cast<std::size_t>(derLength),
             message_,
             message.size()
         ) == 1;
}

// A COSE_Key as it appears in a proof, before it is matched to the proof's algorithm.
struct CoseKey {
  int64_t kty{0};
  int64_t crv{0};
  std::string x;
  std::string y;
  bool hasY{false};
};

CoseKey readCoseKey(CborReader& reader) {
  uint64_t count = 0;
  require(reader.readMapLen(count) && (count == 3 || count == 4));
  CoseKey key;
  std::unordered_set<int64_t> seen;
  for (uint64_t i = 0; i < count; ++i) {
    int64_t label = 0;
    require(reader.readInt(label) && seen.insert(label).second);
    if (label == 1) {
      require(reader.readInt(key.kty));
    } else if (label == -1) {
      require(reader.readInt(key.crv));
    } else if (label == -2 || label == -3) {
      std::string_view coordinate;
      require(reader.readByteString(coordinate) && coordinate.size() <= 66);
      (label == -2 ? key.x : key.y) = std::string(coordinate);
      key.hasY = key.hasY || label == -3;
    } else {
      require(false);
    }
  }
  require(seen.contains(1) && seen.contains(-1) && seen.contains(-2));
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

// The key the proof's algorithm needs, or a rejection when the key is of another type.
DpopKey keyForAlgorithm(int64_t algorithm, const CoseKey& key) {
  const auto* shape = shapeForAlgorithm(algorithm);
  if (shape == nullptr) {
    reject("unsupported DPoP proof algorithm"); // Symmetric and other algorithms are not allowed.
  }
  if (key.kty != shape->kty || key.crv != shape->crv || key.x.size() != shape->size ||
      (shape->group != nullptr ? (!key.hasY || key.y.size() != shape->size) : key.hasY)) {
    reject("DPoP key does not match the proof algorithm");
  }
  return DpopKey{algorithm, key.x, key.y};
}

// Deterministic CBOR of the required members, in bytewise key order (RFC 9679 section 3).
std::string encodeCoseKey(const DpopKey& key) {
  const auto& shape = requireShape(key.algorithm);
  std::string out;
  appendHead(out, 5, shape.group != nullptr ? 4 : 3);
  appendInt(out, 1);
  appendInt(out, shape.kty);
  appendInt(out, -1);
  appendInt(out, shape.crv);
  appendInt(out, -2);
  appendString(out, 2, key.x);
  if (shape.group != nullptr) {
    appendInt(out, -3);
    appendString(out, 2, key.y);
  }
  return out;
}

struct ProofClaims {
  std::chrono::system_clock::time_point issuedAt;
  int64_t issuedAtSeconds{0};
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
      // Reject what the clock cannot represent before converting (as C4m.cpp readTime does).
      using Clock = std::chrono::system_clock;
      using Seconds = std::chrono::seconds;
      require(
          seconds >= std::chrono::duration_cast<Seconds>(Clock::duration::min()).count() &&
          seconds <= std::chrono::duration_cast<Seconds>(Clock::duration::max()).count()
      );
      claims.issuedAtSeconds = seconds;
      claims.issuedAt = Clock::time_point(Seconds(seconds));
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

std::string coseKeyThumbprint(const DpopKey& key) {
  return sha256(encodeCoseKey(key));
}

std::string jwkThumbprint(const DpopKey& key) {
  // RFC 7638: required members in lexicographic order, no whitespace. RFC 8037 for OKP.
  const auto& shape = requireShape(key.algorithm);
  std::string json = std::string(R"({"crv":")") + shape.jwkCrv + R"(","kty":")" + shape.jwkKty +
                     R"(","x":")" + base64Url(key.x) + '"';
  if (shape.group != nullptr) {
    json += R"(,"y":")" + base64Url(key.y) + '"';
  }
  return sha256(json + '}');
}

std::string coseKeyThumbprint(std::string_view x, std::string_view y) {
  return coseKeyThumbprint(DpopKey{kDpopAlgEs256, std::string(x), std::string(y)});
}

std::string jwkThumbprint(std::string_view x, std::string_view y) {
  return jwkThumbprint(DpopKey{kDpopAlgEs256, std::string(x), std::string(y)});
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
  std::optional<CoseKey> coseKey;
  int64_t algorithm = 0;
  for (uint64_t i = 0; i < entries; ++i) {
    int64_t label = 0;
    require(header.readInt(label) && seen.insert(label).second);
    if (label == 1) {
      require(header.readInt(algorithm));
      // ES256 (MUST), ES384 and EdDSA (SHOULD). Symmetric algorithms are forbidden.
      if (shapeForAlgorithm(algorithm) == nullptr) {
        reject("unsupported DPoP proof algorithm");
      }
    } else if (label == 16) {
      std::string_view type;
      require(header.readTextString(type));
      if (type != kProofType) {
        reject("DPoP proof has the wrong typ");
      }
    } else if (label == 14) {
      coseKey = readKccs(header);
    } else {
      require(false);
    }
  }
  require(header.eof() && seen.contains(1) && seen.contains(16) && coseKey.has_value());

  // The key must be of the type the algorithm needs, and match the token's cnf, before it is
  // trusted to sign anything.
  const auto key = keyForAlgorithm(algorithm, *coseKey);
  const auto thumbprint = requirement.binding.kind == DpopBinding::Kind::Ckt
                              ? coseKeyThumbprint(key)
                              : jwkThumbprint(key);
  if (thumbprint != requirement.binding.thumbprint) {
    reject("DPoP key does not match token binding");
  }
  const auto publicKey = publicKeyFromKey(key);
  if (!verifyProofSignature(
          key,
          publicKey.get(),
          signatureInput(protectedHeader, payload),
          signature
      )) {
    reject("invalid DPoP proof signature");
  }

  const auto claims = readProofClaims(payload);
  if (claims.athHash != tokenHash) {
    reject("DPoP proof is bound to a different token");
  }
  // Seconds, not clock ticks: both values are in range, so their difference cannot overflow.
  const auto nowSeconds =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
  const auto skewSeconds = claims.issuedAtSeconds > nowSeconds
                               ? claims.issuedAtSeconds - nowSeconds
                               : nowSeconds - claims.issuedAtSeconds;
  if (skewSeconds > requirement.window.count()) {
    reject("DPoP proof is outside the freshness window");
  }
  if (claims.actionType != "moqt" || claims.action != static_cast<uint64_t>(context.action) ||
      claims.trackNamespace != context.trackNamespace || claims.trackName != context.trackName) {
    reject("DPoP proof does not match the requested action");
  }
  return VerifiedDpopProof{claims.cti, claims.issuedAt};
}

namespace {

PkeyPtr readPemKey(const std::string& path, bool privateKey) {
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new_file(path.c_str(), "r"), BIO_free);
  if (!bio) {
    throw std::invalid_argument("cannot read key file: " + path);
  }
  const auto noPassword = [](char*, int, int, void*) { return 0; };
  PkeyPtr key(PEM_read_bio_PrivateKey(bio.get(), nullptr, noPassword, nullptr), EVP_PKEY_free);
  if (!key && !privateKey) {
    BIO_seek(bio.get(), 0);
    key.reset(PEM_read_bio_PUBKEY(bio.get(), nullptr, noPassword, nullptr));
  }
  if (!key) {
    throw std::invalid_argument("not an unencrypted PEM key: " + path);
  }
  return key;
}

DpopKey dpopKeyOf(EVP_PKEY* key, const std::string& path) {
  if (EVP_PKEY_id(key) == EVP_PKEY_ED25519) {
    unsigned char raw[32];
    std::size_t length = sizeof(raw);
    if (EVP_PKEY_get_raw_public_key(key, raw, &length) != 1 || length != sizeof(raw)) {
      throw std::invalid_argument("cannot read the Ed25519 public key: " + path);
    }
    return DpopKey{kDpopAlgEdDsa, std::string(reinterpret_cast<char*>(raw), length), {}};
  }
  char group[64];
  std::size_t groupLength = 0;
  if (EVP_PKEY_id(key) != EVP_PKEY_EC || EVP_PKEY_get_utf8_string_param(
                                             key,
                                             OSSL_PKEY_PARAM_GROUP_NAME,
                                             group,
                                             sizeof(group),
                                             &groupLength
                                         ) != 1) {
    throw std::invalid_argument("not an EC P-256, EC P-384, or Ed25519 key: " + path);
  }
  for (const auto& shape : kShapes) {
    if (shape.group != nullptr && std::string_view(group, groupLength) == shape.group) {
      // Ask for the affine coordinates rather than the encoded point: a key may store its point
      // compressed or uncompressed, and OpenSSL hands the "pub" parameter back in that form.
      BIGNUM* rawX = nullptr;
      BIGNUM* rawY = nullptr;
      const bool read = EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_EC_PUB_X, &rawX) == 1 &&
                        EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_EC_PUB_Y, &rawY) == 1;
      std::unique_ptr<BIGNUM, decltype(&BN_free)> x(rawX, BN_free);
      std::unique_ptr<BIGNUM, decltype(&BN_free)> y(rawY, BN_free);
      std::string xBytes(shape.size, '\0');
      std::string yBytes(shape.size, '\0');
      const int size = static_cast<int>(shape.size);
      if (!read ||
          BN_bn2binpad(x.get(), reinterpret_cast<unsigned char*>(xBytes.data()), size) != size ||
          BN_bn2binpad(y.get(), reinterpret_cast<unsigned char*>(yBytes.data()), size) != size) {
        throw std::invalid_argument("cannot read the EC public key: " + path);
      }
      return DpopKey{shape.algorithm, std::move(xBytes), std::move(yBytes)};
    }
  }
  throw std::invalid_argument("only P-256, P-384 and Ed25519 keys are supported: " + path);
}

} // namespace

DpopKey dpopPublicKey(const std::string& pemPath) {
  const auto key = readPemKey(pemPath, false);
  return dpopKeyOf(key.get(), pemPath);
}

std::pair<std::string, std::string> es256PublicCoordinates(const std::string& pemPath) {
  auto key = dpopPublicKey(pemPath);
  if (key.algorithm != kDpopAlgEs256) {
    throw std::invalid_argument("not an unencrypted PEM P-256 key: " + pemPath);
  }
  return {std::move(key.x), std::move(key.y)};
}

std::string signDpopProof(
    const std::string& privateKeyPemPath,
    std::string_view token,
    const DpopContext& context,
    std::string_view cti,
    std::chrono::system_clock::time_point issuedAt
) {
  return signDpopProofAtSeconds(
      privateKeyPemPath,
      token,
      context,
      cti,
      std::chrono::duration_cast<std::chrono::seconds>(issuedAt.time_since_epoch()).count()
  );
}

std::string signDpopProofAtSeconds(
    const std::string& privateKeyPemPath,
    std::string_view token,
    const DpopContext& context,
    std::string_view cti,
    int64_t issuedAtSeconds
) {
  const auto pkey = readPemKey(privateKeyPemPath, true);
  const auto key = dpopKeyOf(pkey.get(), privateKeyPemPath);
  const auto& shape = requireShape(key.algorithm);

  std::string protectedHeader;
  appendHead(protectedHeader, 5, 3);
  appendInt(protectedHeader, 1);
  appendInt(protectedHeader, key.algorithm);
  appendInt(protectedHeader, 16);
  appendString(protectedHeader, 3, kProofType);
  appendInt(protectedHeader, 14);
  appendHead(protectedHeader, 5, 1);
  appendInt(protectedHeader, 8);
  appendHead(protectedHeader, 5, 1);
  appendInt(protectedHeader, 1);
  protectedHeader += encodeCoseKey(key);

  std::string payload;
  appendHead(payload, 5, 4);
  appendInt(payload, 6);
  appendInt(payload, issuedAtSeconds);
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
  // Ed25519 signs the message itself; ECDSA signs its ES256/ES384 digest.
  if (!ctx ||
      EVP_DigestSignInit(ctx.get(), nullptr, digestFor(key.algorithm), nullptr, pkey.get()) != 1) {
    throw std::runtime_error("cannot sign DPoP proof");
  }
  std::size_t signatureLength = 0;
  const auto* messageBytes = reinterpret_cast<const unsigned char*>(message.data());
  if (EVP_DigestSign(ctx.get(), nullptr, &signatureLength, messageBytes, message.size()) != 1) {
    throw std::runtime_error("cannot sign DPoP proof");
  }
  std::string signed_(signatureLength, '\0');
  if (EVP_DigestSign(
          ctx.get(),
          reinterpret_cast<unsigned char*>(signed_.data()),
          &signatureLength,
          messageBytes,
          message.size()
      ) != 1) {
    throw std::runtime_error("cannot sign DPoP proof");
  }
  signed_.resize(signatureLength);

  std::string raw;
  if (shape.group == nullptr) {
    raw = std::move(signed_); // Ed25519: already the 64 raw signature bytes.
  } else {
    // ECDSA: DER (r, s) to the fixed-width r || s of COSE.
    const auto* derBytes = reinterpret_cast<const unsigned char*>(signed_.data());
    std::unique_ptr<ECDSA_SIG, decltype(&ECDSA_SIG_free)> sig(
        d2i_ECDSA_SIG(nullptr, &derBytes, static_cast<long>(signed_.size())),
        ECDSA_SIG_free
    );
    const BIGNUM* r = nullptr;
    const BIGNUM* s = nullptr;
    if (!sig) {
      throw std::runtime_error("cannot parse DPoP signature");
    }
    ECDSA_SIG_get0(sig.get(), &r, &s);
    raw.assign(2 * shape.size, '\0');
    auto* rawBytes = reinterpret_cast<unsigned char*>(raw.data());
    const int size = static_cast<int>(shape.size);
    if (BN_bn2binpad(r, rawBytes, size) != size || BN_bn2binpad(s, rawBytes + size, size) != size) {
      throw std::runtime_error("cannot encode DPoP signature");
    }
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
