/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "auth/C4m.h"

#include "auth/CborReader.h"

#include <catapult/crypto.hpp>
#include <catapult/error.hpp>
#include <openssl/ecdsa.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <unordered_set>

namespace openmoq::moqx::auth {
namespace {

constexpr size_t kMaxTokenBytes = 65536;

[[noreturn]] void malformed() {
  throw C4mError(AuthError::Malformed, "malformed C4M token");
}

[[noreturn]] void unsupported() {
  throw C4mError(AuthError::UnsupportedClaim, "unsupported C4M restriction");
}

void require(bool condition) {
  if (!condition) {
    malformed();
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

void appendBytes(std::string& out, std::string_view bytes, uint8_t major = 2) {
  appendHead(out, major, bytes.size());
  out.append(bytes);
}

std::string cryptoInput(int64_t algorithm, std::string_view header, std::string_view payload) {
  // RFC 9052: Mac0 and Sign1 use different authenticated context strings.
  std::string out(1, '\x84');
  appendBytes(out, algorithm == 5 ? "MAC0" : "Signature1", 3);
  appendBytes(out, header);
  appendBytes(out, {}); // external_aad
  appendBytes(out, payload);
  return out;
}

std::span<const uint8_t> asBytes(std::string_view value) {
  return {reinterpret_cast<const uint8_t*>(value.data()), value.size()};
}

catapult::HmacSha256Algorithm hmacAlgorithm(std::span<const uint8_t> key) {
  if (key.size() < 32 || key.size() > 64) {
    throw std::invalid_argument("C4M HMAC-SHA256 requires 32 to 64 key bytes");
  }
  return catapult::HmacSha256Algorithm(std::vector<uint8_t>(key.begin(), key.end()));
}

std::vector<uint8_t> signatureToDer(std::string_view raw) {
  require(raw.size() == 64);
  using BnPtr = std::unique_ptr<BIGNUM, decltype(&BN_free)>;
  BnPtr r(BN_bin2bn(reinterpret_cast<const unsigned char*>(raw.data()), 32, nullptr), BN_free);
  BnPtr s(BN_bin2bn(reinterpret_cast<const unsigned char*>(raw.data()) + 32, 32, nullptr), BN_free);
  std::unique_ptr<ECDSA_SIG, decltype(&ECDSA_SIG_free)> signature(ECDSA_SIG_new(), ECDSA_SIG_free);
  if (!r || !s || !signature || ECDSA_SIG_set0(signature.get(), r.get(), s.get()) != 1) {
    throw std::runtime_error("cannot construct ES256 signature");
  }
  r.release();
  s.release();
  const int length = i2d_ECDSA_SIG(signature.get(), nullptr);
  require(length > 0);
  std::vector<uint8_t> der(static_cast<size_t>(length));
  auto* output = der.data();
  require(i2d_ECDSA_SIG(signature.get(), &output) == length);
  return der;
}

std::vector<uint8_t> signatureToCose(std::span<const uint8_t> der) {
  const auto* input = der.data();
  std::unique_ptr<ECDSA_SIG, decltype(&ECDSA_SIG_free)> signature(
      d2i_ECDSA_SIG(nullptr, &input, static_cast<long>(der.size())),
      ECDSA_SIG_free
  );
  require(signature && input == der.data() + der.size());
  const BIGNUM* r = nullptr;
  const BIGNUM* s = nullptr;
  ECDSA_SIG_get0(signature.get(), &r, &s);
  std::vector<uint8_t> raw(64);
  require(BN_bn2binpad(r, raw.data(), 32) == 32 && BN_bn2binpad(s, raw.data() + 32, 32) == 32);
  return raw;
}

struct Envelope : C4mHeader {
  std::string_view protectedHeader;
  std::string_view payload;
  std::string_view tag;
};

void readHeaders(CborReader& reader, bool protectedMap, Envelope& envelope) {
  uint64_t count = 0;
  require(reader.readMapLen(count) && count <= reader.remaining() / 2);
  std::unordered_set<int64_t> seen;
  bool hasAlgorithm = false;
  for (uint64_t i = 0; i < count; ++i) {
    int64_t label = 0;
    require(reader.readInt(label) && seen.insert(label).second);
    if (label == 1) {
      int64_t algorithm = 0;
      require(protectedMap && reader.readInt(algorithm) && (algorithm == 5 || algorithm == -7));
      envelope.algorithm = algorithm;
      hasAlgorithm = true;
    } else if (label == 2) {
      // No critical extensions are implemented in the bearer profile.
      unsupported();
    } else if (label == 4) {
      std::string_view id;
      require(!envelope.keyId && reader.readByteString(id));
      envelope.keyId = std::string(id);
    } else if (label == 16) {
      std::string_view type;
      require(protectedMap && reader.readTextString(type) && type == "CAT");
    } else {
      require(reader.skip());
    }
  }
  require(!protectedMap || hasAlgorithm);
}

Envelope readEnvelope(std::string_view token) {
  require(!token.empty() && token.size() <= kMaxTokenBytes);
  CborReader reader(token);
  uint64_t tag = 0;
  require(reader.readTag(tag));
  if (tag == 61) { // Optional outer CWT tag (RFC 8392).
    require(reader.readTag(tag));
  }
  require(tag == 17 || tag == 18);
  uint64_t length = 0;
  Envelope envelope;
  require(
      reader.readArrayLen(length) && length == 4 && reader.readByteString(envelope.protectedHeader)
  );
  CborReader header(envelope.protectedHeader);
  readHeaders(header, true, envelope);
  require(
      header.eof() &&
      ((tag == 17 && envelope.algorithm == 5) || (tag == 18 && envelope.algorithm == -7))
  );
  readHeaders(reader, false, envelope);
  require(
      reader.readByteString(envelope.payload) && reader.readByteString(envelope.tag) &&
      envelope.tag.size() == (envelope.algorithm == 5 ? 32 : 64) && reader.eof()
  );
  return envelope;
}

uint64_t arrayLength(CborReader& reader, uint64_t min, uint64_t max) {
  uint64_t length = 0;
  require(
      reader.readArrayLen(length) && length >= min && length <= max && length <= reader.remaining()
  );
  return length;
}

MatchRule readMatch(CborReader& reader) {
  MatchRule match;
  if (reader.peekMajor() == cbor::kMajorArray) {
    (void)arrayLength(reader, 2, 2);
    uint64_t type = 0;
    require(reader.readUInt(type) && (type == 1 || type == 2));
    match.type = static_cast<MatchRule::Type>(type);
  }
  std::string_view value;
  require(reader.readByteString(value));
  match.value = std::string(value);
  return match;
}

Scope readScope(CborReader& reader) {
  const auto length = arrayLength(reader, 1, 3);
  Scope scope;
  const auto actions = arrayLength(reader, 1, kMaxTokenBytes);
  for (uint64_t i = 0; i < actions; ++i) {
    int64_t action = 0;
    require(reader.readInt(action));
    // Unrecognized actions never grant a known operation.
    if (action >= 1 && action <= 9) {
      scope.actions.push_back(static_cast<Action>(action));
    }
  }
  if (length >= 2) {
    const auto fields = arrayLength(reader, 1, 33);
    for (uint64_t i = 0; i < fields; ++i) {
      if (reader.readNull()) {
        require(i + 1 == fields);
        scope.exactNamespace = true;
      } else {
        require(scope.namespaceMatches.size() < 32);
        scope.namespaceMatches.push_back(readMatch(reader));
      }
    }
  }
  if (length == 3) {
    scope.trackMatches.push_back(readMatch(reader));
  }
  return scope;
}

std::vector<Scope> readMoqt(CborReader& reader) {
  uint64_t count = 0;
  require(reader.readMapLen(count) && count >= 1 && count <= reader.remaining() / 2);
  if (count != 1) {
    unsupported();
  }
  int64_t key = 0;
  require(reader.readInt(key));
  if (key != 0) {
    unsupported();
  }
  const auto length = arrayLength(reader, 1, 1024);
  std::vector<Scope> scopes;
  scopes.reserve(static_cast<size_t>(length));
  for (uint64_t i = 0; i < length; ++i) {
    scopes.push_back(readScope(reader));
  }
  return scopes;
}

std::chrono::system_clock::time_point readTime(CborReader& reader) {
  int64_t seconds = 0;
  require(reader.readInt(seconds));
  using Clock = std::chrono::system_clock;
  using Seconds = std::chrono::seconds;
  require(
      seconds >= std::chrono::duration_cast<Seconds>(Clock::duration::min()).count() &&
      seconds <= std::chrono::duration_cast<Seconds>(Clock::duration::max()).count()
  );
  return Clock::time_point(Seconds(seconds));
}

Grants readClaims(
    std::string_view payload,
    std::chrono::system_clock::time_point now,
    const ClaimPolicy& policy
) {
  CborReader reader(payload);
  uint64_t count = 0;
  require(reader.readMapLen(count) && count <= reader.remaining() / 2);
  std::unordered_set<int64_t> seen;
  Grants grants;
  std::optional<std::chrono::system_clock::time_point> notBefore;
  bool hasInformationalClaims = false;
  bool hasCti = false;
  for (uint64_t i = 0; i < count; ++i) {
    int64_t key = 0;
    if (reader.peekMajor() == cbor::kMajorTextString) {
      unsupported(); // Includes the appendix's provisional composite claim labels.
    }
    require(reader.readInt(key) && seen.insert(key).second);
    if (key == 4) {
      grants.expiresAt = readTime(reader);
    } else if (key == 5) {
      notBefore = readTime(reader);
    } else if (key == kC4mMoqtClaim) {
      grants.scopes = readMoqt(reader);
    } else if (key == 308) {
      uint64_t value = 0;
      if (!reader.readUInt(value) || value > 1) {
        unsupported(); // catreplay 2 is implementation-defined; fail closed.
      }
      grants.replayProtected = value == 1;
    } else if (key == kC4mRevalidationClaim) {
      uint64_t value = 0;
      require(reader.readUInt(value));
      if (value != 0) {
        // Shorter than the relay can honor, or revalidation disabled: reject (section 2.2).
        if (policy.minRevalidation.count() <= 0 ||
            value < static_cast<uint64_t>(policy.minRevalidation.count()) || value > 31536000) {
          unsupported();
        }
        grants.revalidateEvery = std::chrono::seconds(value);
      }
    } else if (key == 310) {
      uint64_t version = 0;
      require(reader.readUInt(version));
      if (version != 1) {
        unsupported();
      }
    } else if (key == 1) {
      std::string_view value;
      require(reader.readTextString(value));
      grants.issuer = std::string(value);
    } else if (key == 3) {
      // RFC 8392: aud is a single StringOrURI or an array of them.
      std::string_view value;
      if (reader.peekMajor() == cbor::kMajorTextString) {
        require(reader.readTextString(value));
        grants.audiences.emplace_back(value);
      } else {
        uint64_t n = 0;
        require(reader.readArrayLen(n) && n >= 1 && n <= 16 && n <= reader.remaining());
        for (uint64_t j = 0; j < n; ++j) {
          require(reader.readTextString(value));
          grants.audiences.emplace_back(value);
        }
      }
    } else if (key == 2) {
      std::string_view value;
      require(reader.readTextString(value));
      hasInformationalClaims = true;
    } else if (key == 6) {
      (void)readTime(reader);
      hasInformationalClaims = true;
    } else if (key == 7) {
      std::string_view value;
      require(reader.readByteString(value));
      grants.cti = std::string(value);
      hasCti = true;
    } else {
      // In particular: cnf, catdpop, replay restrictions, and old moqt labels.
      // Skipping an unknown restriction can turn a scoped token into a broader grant.
      unsupported();
    }
  }
  require(reader.eof());
  if (grants.expiresAt <= now) {
    throw C4mError(AuthError::Expired, "expired C4M token");
  }
  if (notBefore && *notBefore > now) {
    throw C4mError(AuthError::NotYetValid, "C4M token not yet valid");
  }
  if (grants.replayProtected &&
      (!grants.cti.has_value() || grants.expiresAt == std::chrono::system_clock::time_point::max()
      )) {
    throw C4mError(AuthError::Malformed, "catreplay token requires cti and exp");
  }
  if (policy.audiences.empty()) {
    if (!grants.audiences.empty()) {
      unsupported(); // No local audience to compare against; do not ignore the restriction.
    }
  } else if (std::none_of(
                 grants.audiences.begin(),
                 grants.audiences.end(),
                 [&](const std::string& aud) {
                   return std::find(policy.audiences.begin(), policy.audiences.end(), aud) !=
                          policy.audiences.end();
                 }
             )) {
    throw C4mError(AuthError::Forbidden, "C4M token audience not accepted");
  }
  if (!policy.issuers.empty()) {
    if (!grants.issuer.has_value() ||
        std::find(policy.issuers.begin(), policy.issuers.end(), *grants.issuer) ==
            policy.issuers.end()) {
      throw C4mError(AuthError::Forbidden, "C4M token issuer not accepted");
    }
  } else if (grants.issuer.has_value()) {
    hasInformationalClaims = true;
  }
  // cti is required and enforced by catreplay 1; otherwise it is informational.
  if (hasCti && !grants.replayProtected) {
    hasInformationalClaims = true;
  }
  if (policy.strict && hasInformationalClaims) {
    unsupported();
  }
  return grants;
}

void appendMatch(std::string& out, const MatchRule& match) {
  if (match.type == MatchRule::Type::Prefix || match.type == MatchRule::Type::Suffix) {
    appendHead(out, 4, 2);
    appendHead(out, 0, static_cast<uint64_t>(match.type));
  } else if (match.type != MatchRule::Type::Exact) {
    throw std::invalid_argument("C4M supports only exact, prefix, and suffix matches");
  }
  appendBytes(out, match.value);
}

std::string encodeClaims(const Grants& grants) {
  const bool expires = grants.expiresAt != std::chrono::system_clock::time_point::max();
  std::string out;
  appendHead(
      out,
      5,
      static_cast<unsigned>(expires) + static_cast<unsigned>(!grants.scopes.empty()) +
          static_cast<unsigned>(grants.issuer.has_value()) +
          static_cast<unsigned>(!grants.audiences.empty()) +
          static_cast<unsigned>(grants.revalidateEvery.count() > 0) +
          static_cast<unsigned>(grants.cti.has_value()) +
          static_cast<unsigned>(grants.replayProtected)
  );
  if (grants.revalidateEvery.count() > 0) {
    appendInt(out, kC4mRevalidationClaim);
    appendInt(out, grants.revalidateEvery.count());
  }
  if (grants.cti) {
    appendInt(out, 7);
    appendHead(out, 2, grants.cti->size());
    out += *grants.cti;
  }
  if (grants.replayProtected) {
    if (!grants.cti || grants.cti->empty() ||
        grants.expiresAt == std::chrono::system_clock::time_point::max()) {
      throw std::invalid_argument("catreplay requires cti and an expiry");
    }
    appendInt(out, 308);
    appendInt(out, 1);
  }
  if (grants.issuer) {
    appendInt(out, 1);
    appendHead(out, 3, grants.issuer->size());
    out += *grants.issuer;
  }
  if (!grants.audiences.empty()) {
    if (grants.audiences.size() > 16) {
      throw std::invalid_argument("too many C4M audiences");
    }
    appendInt(out, 3);
    appendHead(out, 4, grants.audiences.size());
    for (const auto& aud : grants.audiences) {
      appendHead(out, 3, aud.size());
      out += aud;
    }
  }
  if (expires) {
    appendInt(out, 4);
    appendInt(
        out,
        std::chrono::duration_cast<std::chrono::seconds>(grants.expiresAt.time_since_epoch())
            .count()
    );
  }
  if (grants.scopes.empty()) {
    return out; // No moqt claim conveys no grants.
  }
  if (grants.scopes.size() > 1024) {
    throw std::invalid_argument("too many C4M scopes");
  }
  appendInt(out, kC4mMoqtClaim);
  appendHead(out, 5, 1);
  appendInt(out, 0);
  appendHead(out, 4, grants.scopes.size());
  for (const auto& scope : grants.scopes) {
    const bool hasNamespace = !scope.namespaceMatches.empty() || scope.exactNamespace;
    if (scope.actions.empty() || scope.namespaceMatches.size() > 32 ||
        scope.trackMatches.size() > 1 || (!scope.trackMatches.empty() && !hasNamespace)) {
      throw std::invalid_argument("scope cannot be represented by the C4M CDDL");
    }
    appendHead(out, 4, !scope.trackMatches.empty() ? 3 : hasNamespace ? 2 : 1);
    appendHead(out, 4, scope.actions.size());
    for (auto action : scope.actions) {
      const auto id = static_cast<uint64_t>(action);
      if (id < 1 || id > 9) {
        throw std::invalid_argument("unknown C4M action");
      }
      appendHead(out, 0, id);
    }
    if (hasNamespace) {
      appendHead(
          out,
          4,
          scope.namespaceMatches.size() + static_cast<unsigned>(scope.exactNamespace)
      );
      for (const auto& match : scope.namespaceMatches) {
        appendMatch(out, match);
      }
      if (scope.exactNamespace) {
        out.push_back('\xf6');
      }
    }
    if (!scope.trackMatches.empty()) {
      appendMatch(out, scope.trackMatches.front());
    }
  }
  return out;
}

bool matches(std::string_view actual, const MatchRule& rule) {
  switch (rule.type) {
  case MatchRule::Type::Exact:
    return actual == rule.value;
  case MatchRule::Type::Prefix:
    return actual.starts_with(rule.value);
  case MatchRule::Type::Suffix:
    return actual.ends_with(rule.value);
  case MatchRule::Type::Contains:
    return false;
  }
  return false;
}

} // namespace

C4mHeader c4mHeader(std::string_view token) {
  auto envelope = readEnvelope(token);
  return C4mHeader{.algorithm = envelope.algorithm, .keyId = std::move(envelope.keyId)};
}

std::optional<std::string> c4mKeyId(std::string_view token) {
  return c4mHeader(token).keyId;
}

Grants verifyC4mToken(
    std::string_view token,
    const catapult::CryptographicAlgorithm& algorithm,
    std::chrono::system_clock::time_point now,
    const ClaimPolicy& policy
) {
  const auto envelope = readEnvelope(token);
  if (envelope.algorithm != algorithm.algorithmId()) {
    throw C4mError(AuthError::BadSignature, "C4M key algorithm does not match protected alg");
  }
  const auto input = cryptoInput(envelope.algorithm, envelope.protectedHeader, envelope.payload);
  bool verified = false;
  try {
    if (envelope.algorithm == -7) {
      const auto signature = signatureToDer(envelope.tag);
      verified = algorithm.verify(asBytes(input), signature);
    } else {
      verified = algorithm.verify(asBytes(input), asBytes(envelope.tag));
    }
  } catch (const catapult::CatError&) {
    throw C4mError(AuthError::BadSignature, "invalid C4M signature/MAC");
  }
  if (!verified) {
    throw C4mError(AuthError::BadSignature, "invalid C4M signature/MAC");
  }
  return readClaims(envelope.payload, now, policy);
}

Grants verifyC4mToken(
    std::string_view token,
    std::span<const uint8_t> key,
    std::chrono::system_clock::time_point now,
    const ClaimPolicy& policy
) {
  const auto algorithm = hmacAlgorithm(key);
  return verifyC4mToken(token, algorithm, now, policy);
}

std::string signC4mToken(
    std::string_view keyId,
    const catapult::CryptographicAlgorithm& algorithm,
    const Grants& grants
) {
  const auto algorithmId = algorithm.algorithmId();
  if (algorithmId != 5 && algorithmId != -7) {
    throw std::invalid_argument("unsupported C4M signing algorithm");
  }
  std::string header;
  appendHead(header, 5, keyId.empty() ? 2 : 3);
  appendInt(header, 1);
  appendInt(header, algorithmId);
  if (!keyId.empty()) {
    appendInt(header, 4);
    appendBytes(header, keyId);
  }
  appendInt(header, 16);
  appendBytes(header, "CAT", 3);
  const auto payload = encodeClaims(grants);
  const auto input = cryptoInput(algorithmId, header, payload);
  auto signature = algorithm.sign(asBytes(input));
  if (algorithmId == -7) {
    // Catapult uses DER internally; COSE ES256 requires fixed-width R || S.
    signature = signatureToCose(signature);
  }
  require(signature.size() == (algorithmId == 5 ? 32 : 64));
  std::string token;
  appendHead(token, 6, algorithmId == 5 ? 17 : 18);
  appendHead(token, 4, 4);
  appendBytes(token, header);
  token.push_back('\xa0');
  appendBytes(token, payload);
  appendBytes(
      token,
      std::string_view(reinterpret_cast<const char*>(signature.data()), signature.size())
  );
  if (token.size() > kMaxTokenBytes) {
    throw std::invalid_argument("C4M token exceeds 64 KiB");
  }
  return token;
}

std::string
signC4mToken(std::string_view keyId, std::span<const uint8_t> key, const Grants& grants) {
  const auto algorithm = hmacAlgorithm(key);
  return signC4mToken(keyId, algorithm, grants);
}

bool matchesC4mScope(
    const Scope& scope,
    Action action,
    std::span<const std::string> trackNamespace,
    std::optional<std::string_view> trackName
) {
  if (std::find(scope.actions.begin(), scope.actions.end(), action) == scope.actions.end() ||
      trackNamespace.size() < scope.namespaceMatches.size() ||
      (scope.exactNamespace && trackNamespace.size() != scope.namespaceMatches.size())) {
    return false;
  }
  for (size_t i = 0; i < scope.namespaceMatches.size(); ++i) {
    if (!matches(trackNamespace[i], scope.namespaceMatches[i])) {
      return false;
    }
  }
  if (scope.trackMatches.empty()) {
    return true;
  }
  return scope.trackMatches.size() == 1 && trackName &&
         matches(*trackName, scope.trackMatches.front());
}

} // namespace openmoq::moqx::auth
