/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "auth/PrivacyPass.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <folly/Conv.h>
#include <folly/logging/xlog.h>

namespace openmoq::moqx::auth {

namespace {

using Clock = std::chrono::system_clock;
using Seconds = std::chrono::seconds;

constexpr size_t kMinUnsignedTokenSize = 4 + 2 + 8 + 8 + 8 + 2 + 2 + 2;
constexpr uint16_t kMaxStringLen = 0xFFFF;
constexpr uint16_t kMaxScopes = 0xFFFF;
constexpr std::string_view kPrivacyPassMagic = "PPv1";

struct PkeyDeleter {
  void operator()(EVP_PKEY* pkey) const { EVP_PKEY_free(pkey); }
};

using PkeyPtr = std::unique_ptr<EVP_PKEY, PkeyDeleter>;

struct MdCtxDeleter {
  void operator()(EVP_MD_CTX* ctx) const { EVP_MD_CTX_free(ctx); }
};

using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, MdCtxDeleter>;

struct BioDeleter {
  void operator()(BIO* bio) const { BIO_free(bio); }
};

using BioPtr = std::unique_ptr<BIO, BioDeleter>;

template <class T> void appendInteger(std::string& out, T value) {
  for (int i = sizeof(T) - 1; i >= 0; --i) {
    out.push_back(static_cast<char>((static_cast<uint64_t>(value) >> (i * 8)) & 0xFF));
  }
}

template <class T> bool readInteger(std::string_view& in, T& value) {
  if (in.size() < sizeof(T)) {
    return false;
  }
  uint64_t out = 0;
  for (size_t i = 0; i < sizeof(T); ++i) {
    out = (out << 8) | static_cast<unsigned char>(in[i]);
  }
  in.remove_prefix(sizeof(T));
  value = static_cast<T>(out);
  return true;
}

bool appendString(std::string& out, std::string_view value) {
  if (value.size() > kMaxStringLen) {
    return false;
  }
  appendInteger<uint16_t>(out, static_cast<uint16_t>(value.size()));
  out.append(value.data(), value.size());
  return true;
}

bool readString(std::string_view& in, std::string& value) {
  uint16_t size = 0;
  if (!readInteger(in, size) || in.size() < size) {
    return false;
  }
  value.assign(in.substr(0, size));
  in.remove_prefix(size);
  return true;
}

bool readStringView(std::string_view& in, std::string_view& value) {
  uint16_t size = 0;
  if (!readInteger(in, size) || in.size() < size) {
    return false;
  }
  value = in.substr(0, size);
  in.remove_prefix(size);
  return true;
}

bool writeMatchSpec(std::string& out, const MatchSpec& spec) {
  out.push_back(static_cast<char>(spec.kind));
  return appendString(out, spec.value);
}

bool readMatchSpec(std::string_view& in, MatchSpec& spec) {
  if (in.empty()) {
    return false;
  }
  spec.kind = static_cast<MatchKind>(static_cast<unsigned char>(in.front()));
  in.remove_prefix(1);
  std::string_view value;
  if (!readStringView(in, value)) {
    return false;
  }
  spec.value.assign(value.data(), value.size());
  return true;
}

std::string encodeClaimsPayload(const Claims& claims) {
  std::string out;
  out.reserve(256);
  appendInteger<uint64_t>(
      out,
      static_cast<uint64_t>(claims.issuedAt.time_since_epoch() / Seconds(1))
  );
  appendInteger<uint64_t>(
      out,
      static_cast<uint64_t>(claims.expiresAt.time_since_epoch() / Seconds(1))
  );
  appendInteger<uint64_t>(
      out,
      static_cast<uint64_t>(claims.revalidateAfter ? claims.revalidateAfter->count() : 0)
  );
  if (!appendString(out, claims.audience)) {
    throw std::runtime_error("audience too large");
  }
  if (claims.scopes.size() > kMaxScopes) {
    throw std::runtime_error("too many scopes");
  }
  appendInteger<uint16_t>(out, static_cast<uint16_t>(claims.scopes.size()));
  for (const auto& scope : claims.scopes) {
    if (scope.actions.size() > 255) {
      throw std::runtime_error("too many actions");
    }
    out.push_back(static_cast<char>(scope.actions.size()));
    for (auto action : scope.actions) {
      out.push_back(static_cast<char>(action));
    }
    if (!writeMatchSpec(out, scope.namespaceMatch) || !writeMatchSpec(out, scope.trackMatch)) {
      throw std::runtime_error("match value too large");
    }
  }
  return out;
}

bool decodeClaimsPayload(std::string_view payload, Claims& claims) {
  uint64_t iat = 0;
  uint64_t exp = 0;
  uint64_t reval = 0;
  if (!readInteger(payload, iat) || !readInteger(payload, exp) || !readInteger(payload, reval)) {
    return false;
  }
  claims.issuedAt = Clock::time_point(Seconds(iat));
  claims.expiresAt = Clock::time_point(Seconds(exp));
  if (reval > 0) {
    claims.revalidateAfter = Seconds(reval);
  } else {
    claims.revalidateAfter = std::nullopt;
  }
  if (!readString(payload, claims.audience)) {
    return false;
  }
  uint16_t scopeCount = 0;
  if (!readInteger(payload, scopeCount)) {
    return false;
  }
  claims.scopes.clear();
  claims.scopes.reserve(scopeCount);
  for (uint16_t i = 0; i < scopeCount; ++i) {
    if (payload.empty()) {
      return false;
    }
    const auto actionCount = static_cast<unsigned char>(payload.front());
    payload.remove_prefix(1);
    Scope scope;
    scope.actions.reserve(actionCount);
    for (unsigned char j = 0; j < actionCount; ++j) {
      if (payload.empty()) {
        return false;
      }
      scope.actions.push_back(static_cast<Action>(static_cast<unsigned char>(payload.front())));
      payload.remove_prefix(1);
    }
    if (!readMatchSpec(payload, scope.namespaceMatch) ||
        !readMatchSpec(payload, scope.trackMatch)) {
      return false;
    }
    claims.scopes.push_back(std::move(scope));
  }
  return payload.empty();
}

PkeyPtr loadPublicKey(const std::string& pem) {
  BioPtr bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
  if (!bio) {
    return nullptr;
  }
  return PkeyPtr(PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr));
}

PkeyPtr loadPrivateKey(const std::string& pem) {
  BioPtr bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
  if (!bio) {
    return nullptr;
  }
  return PkeyPtr(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
}

std::string signPayload(const std::string& payload, EVP_PKEY* key) {
  auto md = MdCtxPtr(EVP_MD_CTX_new());
  if (!md) {
    throw std::runtime_error("EVP_MD_CTX_new failed");
  }
  if (EVP_DigestSignInit(md.get(), nullptr, nullptr, nullptr, key) != 1) {
    throw std::runtime_error("EVP_DigestSignInit failed");
  }
  size_t sigLen = 0;
  if (EVP_DigestSign(
          md.get(),
          nullptr,
          &sigLen,
          reinterpret_cast<const unsigned char*>(payload.data()),
          payload.size()
      ) != 1) {
    throw std::runtime_error("EVP_DigestSign(size) failed");
  }
  std::string sig(sigLen, '\0');
  if (EVP_DigestSign(
          md.get(),
          reinterpret_cast<unsigned char*>(sig.data()),
          &sigLen,
          reinterpret_cast<const unsigned char*>(payload.data()),
          payload.size()
      ) != 1) {
    throw std::runtime_error("EVP_DigestSign failed");
  }
  sig.resize(sigLen);
  return sig;
}

bool verifyPayload(const std::string& payload, std::string_view signature, EVP_PKEY* key) {
  auto md = MdCtxPtr(EVP_MD_CTX_new());
  if (!md) {
    return false;
  }
  if (EVP_DigestVerifyInit(md.get(), nullptr, nullptr, nullptr, key) != 1) {
    return false;
  }
  return EVP_DigestVerify(
             md.get(),
             reinterpret_cast<const unsigned char*>(signature.data()),
             signature.size(),
             reinterpret_cast<const unsigned char*>(payload.data()),
             payload.size()
         ) == 1;
}

std::string makeTokenWireFormat(
    const config::AuthIssuerKey& issuerKey,
    std::string_view privateKeyPem,
    const Claims& claims
) {
  const auto claimsPayload = encodeClaimsPayload(claims);
  std::string signedPortion;
  signedPortion.reserve(4 + 2 + issuerKey.id.size() + claimsPayload.size());
  signedPortion.append(kPrivacyPassMagic.data(), kPrivacyPassMagic.size());
  if (!appendString(signedPortion, issuerKey.id)) {
    throw std::runtime_error("issuer key id too large");
  }
  appendInteger<uint16_t>(signedPortion, static_cast<uint16_t>(claimsPayload.size()));
  signedPortion.append(claimsPayload);
  auto key = loadPrivateKey(std::string(privateKeyPem));
  if (!key) {
    throw std::runtime_error("failed to load private key");
  }
  auto signature = signPayload(signedPortion, key.get());

  std::string token = signedPortion;
  appendInteger<uint16_t>(token, static_cast<uint16_t>(signature.size()));
  token.append(signature);
  return token;
}

std::optional<std::string_view> maybePrivacyPassTokenValue(const moxygen::AuthToken& token) {
  if (token.tokenValue.size() < kPrivacyPassMagic.size() ||
      token.tokenValue.compare(0, kPrivacyPassMagic.size(), kPrivacyPassMagic) != 0) {
    return std::nullopt;
  }
  return std::string_view(token.tokenValue);
}

} // namespace

bool MatchSpec::matches(std::string_view target) const {
  switch (kind) {
  case MatchKind::Any:
    return true;
  case MatchKind::Exact:
    return target == value;
  case MatchKind::Prefix:
    return target.size() >= value.size() && target.substr(0, value.size()) == value;
  case MatchKind::Suffix:
    return target.size() >= value.size() &&
           target.substr(target.size() - value.size(), value.size()) == value;
  case MatchKind::Contains:
    return target.find(value) != std::string_view::npos;
  }
  return false;
}

std::string toString(AuthError error) {
  switch (error) {
  case AuthError::Missing:
    return "missing";
  case AuthError::Malformed:
    return "malformed";
  case AuthError::BadSignature:
    return "bad signature";
  case AuthError::Expired:
    return "expired";
  case AuthError::Forbidden:
    return "forbidden";
  case AuthError::WrongTokenType:
    return "wrong token type";
  }
  return "unknown";
}

std::string signTokenForTest(
    const config::AuthIssuerKey& issuerKey,
    std::string_view privateKeyPem,
    const Claims& claims
) {
  return makeTokenWireFormat(issuerKey, privateKeyPem, claims);
}

PrivacyPassVerifier::PrivacyPassVerifier(config::AuthConfig config) : config_(std::move(config)) {
  issuerKeys_ = config_.issuerKeys;
}

folly::Expected<Claims, AuthError>
PrivacyPassVerifier::tryVerifyToken(const moxygen::AuthToken& token) const {
  auto wire = maybePrivacyPassTokenValue(token);
  if (!wire) {
    return folly::makeUnexpected(AuthError::WrongTokenType);
  }

  std::string_view view = *wire;
  if (view.size() < kMinUnsignedTokenSize ||
      view.substr(0, kPrivacyPassMagic.size()) != kPrivacyPassMagic) {
    return folly::makeUnexpected(AuthError::WrongTokenType);
  }
  view.remove_prefix(kPrivacyPassMagic.size());

  std::string issuerId;
  if (!readString(view, issuerId) || view.size() < sizeof(uint16_t)) {
    return folly::makeUnexpected(AuthError::Malformed);
  }
  uint16_t claimsSize = 0;
  if (!readInteger(view, claimsSize) || view.size() < claimsSize + sizeof(uint16_t)) {
    return folly::makeUnexpected(AuthError::Malformed);
  }
  std::string claimsPayload(view.substr(0, claimsSize));
  view.remove_prefix(claimsSize);

  uint16_t signatureSize = 0;
  if (!readInteger(view, signatureSize) || view.size() != signatureSize) {
    return folly::makeUnexpected(AuthError::Malformed);
  }
  std::string_view signature = view;

  const auto keyIt = std::find_if(issuerKeys_.begin(), issuerKeys_.end(), [&](const auto& key) {
    return key.id == issuerId;
  });
  if (keyIt == issuerKeys_.end()) {
    return folly::makeUnexpected(AuthError::WrongTokenType);
  }
  const auto signedLen = wire->size() - signatureSize - sizeof(uint16_t);
  auto key = loadPublicKey(keyIt->publicKeyPem);
  if (!key) {
    return folly::makeUnexpected(AuthError::Malformed);
  }
  if (!verifyPayload(std::string(wire->substr(0, signedLen)), signature, key.get())) {
    return folly::makeUnexpected(AuthError::BadSignature);
  }

  Claims claims;
  if (!decodeClaimsPayload(claimsPayload, claims)) {
    return folly::makeUnexpected(AuthError::Malformed);
  }
  claims.issuerId = issuerId;
  return claims;
}

folly::Expected<Claims, AuthError> PrivacyPassVerifier::verify(const moxygen::AuthToken& token
) const {
  if (!config_.enabled) {
    return folly::makeUnexpected(AuthError::WrongTokenType);
  }
  auto claims = tryVerifyToken(token);
  if (!claims.hasValue()) {
    return folly::makeUnexpected(claims.error());
  }

  const auto now = Clock::now();
  if (claims->audience != config_.audience) {
    return folly::makeUnexpected(AuthError::Forbidden);
  }
  if (now < claims->issuedAt || now >= claims->expiresAt) {
    return folly::makeUnexpected(AuthError::Expired);
  }
  if (claims->revalidateAfter) {
    auto revalidateAt = claims->issuedAt + *claims->revalidateAfter;
    if (now >= revalidateAt) {
      return folly::makeUnexpected(AuthError::Expired);
    }
  }
  return *claims;
}

bool PrivacyPassVerifier::allows(
    const Claims& claims,
    Action action,
    std::string_view namespaceValue,
    std::string_view trackValue
) const {
  const bool isSetupAction = action == Action::ClientSetup || action == Action::ServerSetup;
  const bool isNamespaceAction =
      action == Action::PublishNamespace || action == Action::SubscribeNamespace;
  const bool isTrackAction = !isSetupAction && !isNamespaceAction;

  for (const auto& scope : claims.scopes) {
    if (std::find(scope.actions.begin(), scope.actions.end(), action) == scope.actions.end()) {
      continue;
    }
    if (!isSetupAction) {
      if (!scope.namespaceMatch.matches(namespaceValue)) {
        continue;
      }
      if (isTrackAction && !scope.trackMatch.matches(trackValue)) {
        continue;
      }
    }
    return true;
  }
  return false;
}

folly::Expected<folly::Unit, AuthError> PrivacyPassVerifier::authorize(
    const moxygen::Parameters& params,
    Action action,
    std::string_view namespaceValue,
    std::string_view trackValue
) const {
  if (!config_.enabled) {
    return folly::unit;
  }

  bool sawPrivacyPassToken = false;
  for (const auto& param : params) {
    if (param.key != folly::to_underlying(moxygen::TrackRequestParamKey::AUTHORIZATION_TOKEN) &&
        param.key != folly::to_underlying(moxygen::SetupKey::AUTHORIZATION_TOKEN)) {
      continue;
    }
    sawPrivacyPassToken = true;
    auto claims = verify(param.asAuthToken);
    if (!claims.hasValue()) {
      if (claims.error() == AuthError::WrongTokenType) {
        continue;
      }
      return folly::makeUnexpected(claims.error());
    }
    if (allows(*claims, action, namespaceValue, trackValue)) {
      return folly::unit;
    }
  }

  if (!sawPrivacyPassToken) {
    return folly::makeUnexpected(AuthError::Missing);
  }
  return folly::makeUnexpected(AuthError::Forbidden);
}

folly::Expected<folly::Unit, AuthError>
PrivacyPassVerifier::authorizeSetup(const moxygen::SetupParameters& params) const {
  if (!config_.enabled) {
    return folly::unit;
  }
  bool sawPrivacyPassToken = false;
  for (const auto& param : params) {
    if (param.key != folly::to_underlying(moxygen::SetupKey::AUTHORIZATION_TOKEN)) {
      continue;
    }
    sawPrivacyPassToken = true;
    auto claims = verify(param.asAuthToken);
    if (!claims.hasValue()) {
      if (claims.error() == AuthError::WrongTokenType) {
        continue;
      }
      return folly::makeUnexpected(claims.error());
    }
    if (allows(*claims, Action::ClientSetup, {})) {
      return folly::unit;
    }
  }
  if (!sawPrivacyPassToken) {
    return folly::unit;
  }
  return folly::makeUnexpected(AuthError::Forbidden);
}

} // namespace openmoq::moqx::auth
