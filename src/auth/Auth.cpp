/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "auth/Auth.h"
#include "auth/CborReader.h"

#include <folly/Conv.h>
#include <folly/Expected.h>
#include <folly/Range.h>
#include <folly/lang/Bits.h>
#include <folly/logging/xlog.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/opensslv.h>
#include <openssl/params.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>

using namespace moxygen;

namespace openmoq::moqx::auth {
namespace {

constexpr uint8_t kEnvelopeVersion = 1;
constexpr size_t kHmacSha256Len = 32;

std::string hmacSha256(std::string_view secret, std::string_view payload) {
#if OPENSSL_VERSION_MAJOR >= 3
  auto* mac = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
  XCHECK(mac) << "EVP_MAC_fetch(HMAC) failed";
  auto macGuard = std::unique_ptr<EVP_MAC, decltype(&EVP_MAC_free)>(mac, EVP_MAC_free);

  auto* ctx = EVP_MAC_CTX_new(macGuard.get());
  XCHECK(ctx) << "EVP_MAC_CTX_new failed";
  auto ctxGuard = std::unique_ptr<EVP_MAC_CTX, decltype(&EVP_MAC_CTX_free)>(ctx, EVP_MAC_CTX_free);

  auto digestName = std::array<char, 7>{'S', 'H', 'A', '2', '5', '6', '\0'};
  OSSL_PARAM params[] = {
      OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digestName.data(), 0),
      OSSL_PARAM_construct_end(),
  };
  XCHECK_EQ(
      EVP_MAC_init(
          ctxGuard.get(),
          reinterpret_cast<const unsigned char*>(secret.data()),
          secret.size(),
          params
      ),
      1
  ) << "EVP_MAC_init failed";
  XCHECK_EQ(
      EVP_MAC_update(
          ctxGuard.get(),
          reinterpret_cast<const unsigned char*>(payload.data()),
          payload.size()
      ),
      1
  ) << "EVP_MAC_update failed";
  size_t len = kHmacSha256Len;
  std::string out(kHmacSha256Len, '\0');
  XCHECK_EQ(
      EVP_MAC_final(ctxGuard.get(), reinterpret_cast<unsigned char*>(out.data()), &len, out.size()),
      1
  ) << "EVP_MAC_final failed";
  out.resize(len);
  return out;
#else
  unsigned int len = 0;
  std::string out(kHmacSha256Len, '\0');
  HMAC(
      EVP_sha256(),
      secret.data(),
      static_cast<int>(secret.size()),
      reinterpret_cast<const unsigned char*>(payload.data()),
      payload.size(),
      reinterpret_cast<unsigned char*>(out.data()),
      &len
  );
  out.resize(len);
  return out;
#endif
}

uint32_t readU32BE(std::string_view data, size_t offset) {
  return (static_cast<uint32_t>(static_cast<unsigned char>(data[offset])) << 24) |
         (static_cast<uint32_t>(static_cast<unsigned char>(data[offset + 1])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(data[offset + 2])) << 8) |
         static_cast<uint32_t>(static_cast<unsigned char>(data[offset + 3]));
}

void appendU32BE(std::string& out, uint32_t value) {
  out.push_back(static_cast<char>((value >> 24) & 0xff));
  out.push_back(static_cast<char>((value >> 16) & 0xff));
  out.push_back(static_cast<char>((value >> 8) & 0xff));
  out.push_back(static_cast<char>(value & 0xff));
}

std::string canonicalNamespace(const TrackNamespace& ns) {
  std::string out;
  for (const auto& field : ns.trackNamespace) {
    appendU32BE(out, static_cast<uint32_t>(field.size()));
    out.append(field);
  }
  return out;
}

bool bytesMatch(std::string_view actual, const MatchRule& rule) {
  const auto expected = std::string_view(rule.value);
  switch (rule.type) {
  case MatchRule::Type::Exact:
    return actual == expected;
  case MatchRule::Type::Prefix:
    return actual.starts_with(expected);
  case MatchRule::Type::Suffix:
    return actual.size() >= expected.size() &&
           actual.substr(actual.size() - expected.size()) == expected;
  case MatchRule::Type::Contains:
    return actual.find(expected) != std::string_view::npos;
  }
  return false;
}

bool allRulesMatch(std::string_view actual, const std::vector<MatchRule>& rules) {
  return std::all_of(rules.begin(), rules.end(), [&](const auto& rule) {
    return bytesMatch(actual, rule);
  });
}

bool parseAction(CborReader& reader, std::vector<Action>& actions) {
  int64_t action = 0;
  if (!reader.readInt(action) || action < 0) {
    return false;
  }
  actions.push_back(static_cast<Action>(static_cast<uint64_t>(action)));
  return true;
}

bool parseActions(CborReader& reader, std::vector<Action>& actions) {
  uint64_t len = 0;
  // Copy the small reader as a backtracking checkpoint: actions may be a scalar or array.
  auto copy = reader;
  if (copy.readArrayLen(len)) {
    reader = copy;
    for (uint64_t i = 0; i < len; ++i) {
      if (!parseAction(reader, actions)) {
        return false;
      }
    }
    return true;
  }
  return parseAction(reader, actions);
}

bool parseMatchRules(CborReader& reader, std::vector<MatchRule>& rules) {
  uint64_t len = 0;
  if (!reader.readMapLen(len)) {
    return false;
  }
  for (uint64_t i = 0; i < len; ++i) {
    int64_t type = 0;
    std::string value;
    if (!reader.readInt(type) || type < 0 || type > 3 || !reader.readBytes(value)) {
      return false;
    }
    rules.push_back(MatchRule{
        .type = static_cast<MatchRule::Type>(static_cast<uint64_t>(type)),
        .value = std::move(value),
    });
  }
  return true;
}

bool parseScope(CborReader& reader, Scope& scope) {
  uint64_t len = 0;
  if (!reader.readArrayLen(len) || len != 3) {
    return false;
  }
  return parseActions(reader, scope.actions) && parseMatchRules(reader, scope.namespaceMatches) &&
         parseMatchRules(reader, scope.trackMatches);
}

bool parseMoqt(CborReader& reader, Grants& grants) {
  uint64_t len = 0;
  if (!reader.readArrayLen(len)) {
    return false;
  }
  for (uint64_t i = 0; i < len; ++i) {
    Scope scope;
    if (!parseScope(reader, scope)) {
      return false;
    }
    grants.scopes.push_back(std::move(scope));
  }
  return true;
}

bool parseClaims(std::string_view data, Grants& grants, bool strictClaims) {
  CborReader reader(data);
  uint64_t len = 0;
  if (!reader.readMapLen(len)) {
    return false;
  }
  for (uint64_t i = 0; i < len; ++i) {
    auto keyReader = reader;
    int64_t intKey = 0;
    std::string textKey;
    bool hasIntKey = keyReader.readInt(intKey);
    bool hasTextKey = false;
    if (hasIntKey) {
      reader = keyReader;
    } else {
      keyReader = reader;
      hasTextKey = keyReader.readBytes(textKey);
      if (!hasTextKey) {
        return false;
      }
      reader = keyReader;
    }

    if ((hasIntKey && intKey == 4) || (hasTextKey && textKey == "exp")) {
      int64_t exp = 0;
      if (!reader.readInt(exp) || exp < 0) {
        return false;
      }
      grants.expiresAt = std::chrono::system_clock::time_point(std::chrono::seconds(exp));
    } else if (hasTextKey && textKey == "moqt") {
      if (!parseMoqt(reader, grants)) {
        return false;
      }
    } else if (hasTextKey && textKey == "moqt-reval") {
      int64_t ignoredReval = 0;
      if (!reader.readInt(ignoredReval) || ignoredReval < 0) {
        return false;
      }
    } else if (strictClaims) {
      return false;
    } else if (!reader.skip()) {
      return false;
    }
  }
  return reader.eof();
}

} // namespace

AuthTokenVerifier::AuthTokenVerifier(config::AuthConfig config) : config_(std::move(config)) {}

folly::Expected<Grants, AuthError> AuthTokenVerifier::verify(const AuthToken& token) const {
  if (!config_.enabled) {
    return Grants{};
  }
  if (token.tokenType != config_.tokenType) {
    return folly::makeUnexpected(AuthError::WrongTokenType);
  }
  const auto raw = std::string_view(token.tokenValue);
  if (raw.size() < 1 + 1 + 4 + kHmacSha256Len || static_cast<uint8_t>(raw[0]) != kEnvelopeVersion) {
    return folly::makeUnexpected(AuthError::Malformed);
  }
  const auto keyLen = static_cast<size_t>(static_cast<unsigned char>(raw[1]));
  if (raw.size() < 1 + 1 + keyLen + 4 + kHmacSha256Len) {
    return folly::makeUnexpected(AuthError::Malformed);
  }
  const auto keyID = raw.substr(2, keyLen);
  const auto claimsLenOffset = 2 + keyLen;
  const auto claimsLen = readU32BE(raw, claimsLenOffset);
  const auto signedLen = claimsLenOffset + 4 + claimsLen;
  if (raw.size() != signedLen + kHmacSha256Len) {
    return folly::makeUnexpected(AuthError::Malformed);
  }

  const auto key =
      std::find_if(config_.hmacKeys.begin(), config_.hmacKeys.end(), [&](const auto& k) {
        return std::string_view(k.id) == keyID;
      });
  if (key == config_.hmacKeys.end()) {
    return folly::makeUnexpected(AuthError::BadSignature);
  }
  const auto expected = hmacSha256(key->secret, raw.substr(0, signedLen));
  if (CRYPTO_memcmp(expected.data(), raw.data() + signedLen, kHmacSha256Len) != 0) {
    return folly::makeUnexpected(AuthError::BadSignature);
  }

  Grants grants;
  if (!parseClaims(raw.substr(claimsLenOffset + 4, claimsLen), grants, config_.strictClaims)) {
    return folly::makeUnexpected(AuthError::Malformed);
  }
  if (grants.expiresAt <= std::chrono::system_clock::now()) {
    return folly::makeUnexpected(AuthError::Expired);
  }
  return grants;
}

std::string AuthTokenVerifier::signForTest(
    std::string_view keyID,
    std::string_view secret,
    std::string_view cborClaims
) {
  XDCHECK_LE(keyID.size(), 255u);
  std::string out;
  out.push_back(static_cast<char>(kEnvelopeVersion));
  out.push_back(static_cast<char>(keyID.size()));
  out.append(keyID);
  appendU32BE(out, static_cast<uint32_t>(cborClaims.size()));
  out.append(cborClaims);
  out.append(hmacSha256(secret, out));
  return out;
}

std::optional<AuthToken> findAuthToken(const Parameters& params, uint64_t tokenType) {
  const auto authKey = folly::to_underlying(TrackRequestParamKey::AUTHORIZATION_TOKEN);
  for (const auto& param : params) {
    if (param.key == authKey && param.asAuthToken.tokenType == tokenType) {
      return param.asAuthToken;
    }
  }
  return std::nullopt;
}

bool allows(
    const Grants& grants,
    Action action,
    const TrackNamespace& ns,
    std::optional<std::string_view> trackName,
    std::chrono::system_clock::time_point now
) {
  if (grants.expiresAt <= now) {
    return false;
  }
  const auto nsBytes = canonicalNamespace(ns);
  const auto trackBytes = trackName.value_or(std::string_view());
  for (const auto& scope : grants.scopes) {
    if (std::find(scope.actions.begin(), scope.actions.end(), action) == scope.actions.end()) {
      continue;
    }
    if (allRulesMatch(nsBytes, scope.namespaceMatches) &&
        allRulesMatch(trackBytes, scope.trackMatches)) {
      return true;
    }
  }
  return false;
}

const char* toString(AuthError error) {
  switch (error) {
  case AuthError::Missing:
    return "missing authorization token";
  case AuthError::WrongTokenType:
    return "wrong authorization token type";
  case AuthError::Malformed:
    return "malformed authorization token";
  case AuthError::BadSignature:
    return "invalid authorization token signature";
  case AuthError::Expired:
    return "expired authorization token";
  case AuthError::Forbidden:
    return "authorization token does not permit action";
  }
  return "authorization failed";
}

} // namespace openmoq::moqx::auth
