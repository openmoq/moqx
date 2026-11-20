/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "auth/C4m.h"
#include "auth/Es256Key.h"
#include "config/Pkcs12TestUtils.h"

#include <catapult/crypto.hpp>
#include <gtest/gtest.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>

#include <array>
#include <string>

using namespace openmoq::moqx::auth;

namespace {

std::string unhex(std::string_view hex) {
  std::string bytes;
  for (size_t i = 0; i < hex.size(); i += 2) {
    bytes.push_back(static_cast<char>(std::stoul(std::string(hex.substr(i, 2)), nullptr, 16)));
  }
  return bytes;
}

const std::array<uint8_t, 32> kKey = [] {
  std::array<uint8_t, 32> key{};
  for (size_t i = 0; i < key.size(); ++i) {
    key[i] = static_cast<uint8_t>(i);
  }
  return key;
}();

const auto kNow = std::chrono::system_clock::time_point(std::chrono::seconds(1700000000));

// Independent test encoder: the production issuer cannot make the verifier's fixtures.
std::string bytes(std::string_view value) {
  std::string out;
  if (value.size() < 24) {
    out.push_back(static_cast<char>(0x40 + value.size()));
  } else {
    out.push_back('\x58');
    out.push_back(static_cast<char>(value.size()));
  }
  return out + std::string(value);
}

std::string rawToken(std::string_view payloadHex, std::string_view headerHex = "a201051063434154") {
  auto header = unhex(headerHex);
  auto payload = unhex(payloadHex);
  auto input = std::string(
                   "\x84\x64"
                   "MAC0",
                   6
               ) +
               bytes(header) + '\x40' + bytes(payload);
  std::array<unsigned char, 32> mac{};
  unsigned len = 0;
  HMAC(
      EVP_sha256(),
      kKey.data(),
      kKey.size(),
      reinterpret_cast<const unsigned char*>(input.data()),
      input.size(),
      mac.data(),
      &len
  );
  return std::string("\xd1\x84", 2) + bytes(header) + '\xa0' + bytes(payload) +
         bytes(std::string_view(reinterpret_cast<const char*>(mac.data()), len));
}

void expectError(std::string_view token, AuthError expected) {
  try {
    (void)verifyC4mToken(token, kKey, kNow);
    FAIL() << "token accepted";
  } catch (const C4mError& error) {
    EXPECT_EQ(error.code(), expected) << error.what();
  }
}

} // namespace

TEST(C4mTest, RecognizesPublishedEs256Sign1Envelope) {
  // draft-ietf-moq-c4m-02 Appendix A.3: token_es256.
  const auto token = unhex("d28448a201261063434154a05849a401781868747470733a2f2f6175"
                           "74682e6578616d706c652e636f6d0381781d68747470733a2f2f6d6f"
                           "712d72656c61792e6578616d706c652e636f6d041a65554280051a65"
                           "53f1005840ed8cf586f32ba410f2b3097642eb32255c8dc555e9e058"
                           "c48084809df557887bc5ee4974a9ef5c4557c1557de799628b634caf"
                           "aa1b74c936968e0c9c3a48f222");
  EXPECT_NO_THROW(EXPECT_FALSE(c4mKeyId(token).has_value()));
  auto publicKey = loadEs256PublicKeyFile(std::string(TEST_C4M_KEY_DIR) + "/c4m_es256_public.pem");
  // The published signature must verify before the unsupported audience claim is rejected.
  try {
    (void)verifyC4mToken(token, *publicKey, kNow);
    FAIL() << "audience restriction must not be ignored";
  } catch (const C4mError& error) {
    EXPECT_EQ(error.code(), AuthError::UnsupportedClaim);
  }
  auto tampered = token;
  tampered.back() ^= 1;
  try {
    (void)verifyC4mToken(tampered, *publicKey, kNow);
    FAIL() << "tampered signature accepted";
  } catch (const C4mError& error) {
    EXPECT_EQ(error.code(), AuthError::BadSignature);
  }
}

TEST(C4mTest, Es256PublicOnlyKeyVerifiesButCannotSign) {
  auto privateKey =
      loadEs256PrivateKeyFile(std::string(TEST_C4M_KEY_DIR) + "/c4m_es256_private.pem");
  auto publicKey = loadEs256PublicKeyFile(std::string(TEST_C4M_KEY_DIR) + "/c4m_es256_public.pem");
  Grants grants;
  grants.expiresAt = kNow + std::chrono::hours(1);
  grants.scopes.push_back(Scope{.actions = {Action::ClientSetup}});
  const auto token = signC4mToken("ec-key", *privateKey, grants);
  ASSERT_FALSE(token.empty());
  EXPECT_EQ(static_cast<uint8_t>(token[0]), 0xd2);
  EXPECT_EQ(c4mHeader(token).algorithm, -7);
  auto decoded = verifyC4mToken(token, *publicKey, kNow);
  ASSERT_EQ(decoded.scopes.size(), 1u);
  EXPECT_TRUE(matchesC4mScope(decoded.scopes[0], Action::ClientSetup, {}, std::nullopt));
  EXPECT_THROW(signC4mToken("ec-key", *publicKey, grants), catapult::CryptoError);
  EXPECT_THROW(verifyC4mToken(token, kKey, kNow), C4mError);
  auto macToken = signC4mToken("ec-key", kKey, grants);
  EXPECT_THROW(verifyC4mToken(macToken, *publicKey, kNow), C4mError);
}

TEST(C4mTest, Es256KeyFilesRejectPrivateMaterialAndOtherCurves) {
  const auto path = std::string(TEST_C4M_KEY_DIR);
  EXPECT_THROW(loadEs256PublicKeyFile(path + "/c4m_es256_private.pem"), std::invalid_argument);
  EXPECT_THROW(loadEs256PrivateKeyFile(path + "/c4m_es256_public.pem"), std::invalid_argument);
  EXPECT_THROW(loadEs256PublicKeyFile(path + "/missing-public-key.pem"), std::runtime_error);
  for (const auto* group : {"secp384r1", "secp256k1"}) {
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(EVP_EC_gen(group), EVP_PKEY_free);
    ASSERT_TRUE(key);
    std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new(BIO_s_mem()), BIO_free);
    ASSERT_TRUE(output);
    ASSERT_EQ(PEM_write_bio_PUBKEY(output.get(), key.get()), 1);
    char* data = nullptr;
    const auto size = BIO_get_mem_data(output.get(), &data);
    openmoq::moqx::config::test::TempFile file(
        std::string_view(data, static_cast<size_t>(size)),
        ".pem"
    );
    EXPECT_THROW(loadEs256PublicKeyFile(file.path()), std::invalid_argument) << group;
  }
}

TEST(C4mTest, VerifiesPublishedPublisherVector) {
  // draft-ietf-moq-c4m-02 Appendix A.5, moqt_publisher_exact.
  const auto token = unhex("d18448a201051063434154a0584aa301781868747470733a2f2f6175"
                           "74682e6578616d706c652e636f6d041a655542803a00010000a10081"
                           "83820206824b6578616d706c652e636f6d45616c6963658201467669"
                           "64656f2d5820467c57ade72d6be629b745ef33d6e28ec38ba1ca8bd3"
                           "feb851f3832af94fe686");
  auto grants = verifyC4mToken(token, kKey, kNow);
  ASSERT_EQ(grants.scopes.size(), 1);
  const std::array<std::string, 2> ns{"example.com", "alice"};
  EXPECT_TRUE(matchesC4mScope(grants.scopes[0], Action::Publish, ns, "video-hd"));
  EXPECT_FALSE(matchesC4mScope(grants.scopes[0], Action::Publish, ns, "audio-main"));
  EXPECT_FALSE(matchesC4mScope(grants.scopes[0], Action::Subscribe, ns, "video-hd"));
  EXPECT_FALSE(c4mKeyId(token).has_value());
}

TEST(C4mTest, NamespaceFieldsAndFinalNilAreEnforced) {
  // {moqt: {0: [[[4], [h'61', [1, h'62'], nil], [2, h'63']]]}}
  auto grants =
      verifyC4mToken(rawToken("a13a00010000a1008183810483416182014162f682024163"), kKey, kNow);
  const auto& scope = grants.scopes.at(0);
  EXPECT_TRUE(
      matchesC4mScope(scope, Action::Subscribe, std::array<std::string, 2>{"a", "bee"}, "abc")
  );
  EXPECT_FALSE(
      matchesC4mScope(scope, Action::Subscribe, std::array<std::string, 3>{"a", "bee", ""}, "abc")
  );
  EXPECT_FALSE(matchesC4mScope(scope, Action::Subscribe, std::array<std::string, 1>{"abee"}, "abc")
  );
  EXPECT_FALSE(
      matchesC4mScope(scope, Action::Subscribe, std::array<std::string, 2>{"a", "bee"}, "abd")
  );
  EXPECT_FALSE(matchesC4mScope(
      scope,
      Action::Subscribe,
      std::array<std::string, 2>{"a", "bee"},
      std::nullopt
  ));
}

TEST(C4mTest, NamespaceWithoutNilIncludesDescendants) {
  auto grants = verifyC4mToken(rawToken("a13a00010000a10081828104814161"), kKey, kNow);
  EXPECT_TRUE(matchesC4mScope(
      grants.scopes.at(0),
      Action::Subscribe,
      std::array<std::string, 2>{"a", "child"},
      "anything"
  ));
  EXPECT_FALSE(matchesC4mScope(
      grants.scopes.at(0),
      Action::Subscribe,
      std::array<std::string, 1>{"ab"},
      "anything"
  ));
}

TEST(C4mTest, RejectsMalformedOrUnsupportedScopeFormats) {
  for (auto payload : {
           "a13a00010000a200818181040100",       // unknown moqt map key
           "a13a00010000a200818181040081818104", // duplicate key 0
           "a13a00010000a10080",                 // empty scopes
           "a13a00010000a100818180",             // empty actions
           "a13a00010000a1008182810480",         // empty namespace array
           "a13a00010000a1008182810482f64161",   // nil before last field
           "a13a00010000a10081828104816161",     // tstr instead of bstr
           "a13a00010000a100818281048182034161", // contains is not a C4M match
           "a119fde8a10081818104",               // legacy catapult claim 65000
       }) {
    SCOPED_TRACE(payload);
    EXPECT_THROW(verifyC4mToken(rawToken(payload), kKey, kNow), C4mError);
  }
}

TEST(C4mTest, RejectsUnsupportedRestrictionsEvenWithoutStrictClaims) {
  for (auto payload : {
           "a108a0",             // cnf
           "a119014101",         // catdpop
           "a119013401",         // catreplay 1
           "a13a0001000119012c", // moqt-reval 300
           "a1036178",           // audience requires deployment-specific matching
           "a119013701",         // CAT network restriction
           "a1636f72a0",         // composite claim
       }) {
    SCOPED_TRACE(payload);
    expectError(rawToken(payload), AuthError::UnsupportedClaim);
  }
  EXPECT_NO_THROW(verifyC4mToken(rawToken("a23a000100010019013400"), kKey, kNow));
}

TEST(C4mTest, ValidatesTimeClaimsAndDuplicateKeys) {
  expectError(rawToken("a1041a6553f100"), AuthError::Expired);
  expectError(rawToken("a1051a6553f101"), AuthError::NotYetValid);
  expectError(rawToken("a2041a65554280041a65554280"), AuthError::Malformed);
  expectError(rawToken("a1046178"), AuthError::Malformed);
  expectError(rawToken("a1041b7fffffffffffffff"), AuthError::Malformed);
}

TEST(C4mTest, RejectsTamperingWrongEnvelopeAndTrailingBytes) {
  auto token = rawToken("a0");
  token.back() ^= 1;
  expectError(token, AuthError::BadSignature);
  token = rawToken("a0");
  token[0] = '\xd2'; // COSE_Sign1 cannot carry an HMAC token
  expectError(token, AuthError::Malformed);
  expectError(rawToken("a0") + '\x00', AuthError::Malformed);
  expectError(rawToken("a000"), AuthError::Malformed);
  expectError(rawToken("a0", "a3010501061063434154"), AuthError::Malformed);
}

TEST(C4mTest, SignedTokensRoundtripWithBinaryKeyIdAndExactNamespace) {
  Grants grants;
  grants.expiresAt = kNow + std::chrono::hours(1);
  grants.scopes.push_back(Scope{
      .actions = {Action::SubscribeTracks},
      .namespaceMatches = {MatchRule{.value = "live"}},
      .exactNamespace = true,
  });
  const std::string kid("key\0id", 6);
  auto token = signC4mToken(kid, kKey, grants);
  EXPECT_EQ(c4mKeyId(token), kid);
  auto decoded = verifyC4mToken(token, kKey, kNow);
  EXPECT_TRUE(matchesC4mScope(
      decoded.scopes.at(0),
      Action::SubscribeTracks,
      std::array<std::string, 1>{"live"},
      std::nullopt
  ));
  EXPECT_FALSE(matchesC4mScope(
      decoded.scopes.at(0),
      Action::SubscribeTracks,
      std::array<std::string, 2>{"live", "child"},
      std::nullopt
  ));
}

TEST(C4mTest, IssuerRejectsLegacyCompoundAndContainsRules) {
  Grants grants;
  grants.scopes.push_back(Scope{
      .actions = {Action::Publish},
      .trackMatches = {MatchRule{.type = MatchRule::Type::Contains, .value = "a"}},
  });
  EXPECT_THROW(signC4mToken("key", kKey, grants), std::invalid_argument);
  grants.scopes[0].trackMatches = {MatchRule{.value = "a"}, MatchRule{.value = "b"}};
  EXPECT_THROW(signC4mToken("key", kKey, grants), std::invalid_argument);
}

TEST(C4mTest, RejectsEveryTruncatedPrefixAndHostileCborLengths) {
  const auto token = rawToken("a13a00010000a10081818101");
  for (size_t size = 0; size < token.size(); ++size) {
    SCOPED_TRACE(size);
    EXPECT_THROW(verifyC4mToken(std::string_view(token).substr(0, size), kKey, kNow), C4mError);
  }
  expectError(rawToken("bb8000000000000000"), AuthError::Malformed);
  expectError(rawToken("a13a00010000a1009bffffffffffffffff"), AuthError::Malformed);
  expectError(std::string(65537, '\x00'), AuthError::Malformed);
}

TEST(C4mTest, PreservesBinaryFieldsAndDistinguishesEmptyTrackFromNoTrack) {
  Scope scope{
      .actions = {Action::Subscribe},
      .namespaceMatches = {MatchRule{.value = std::string("a\0b", 3)}},
      .trackMatches = {MatchRule{.value = ""}},
      .exactNamespace = true,
  };
  const std::array<std::string, 1> ns{std::string("a\0b", 3)};
  EXPECT_TRUE(matchesC4mScope(scope, Action::Subscribe, ns, ""));
  EXPECT_FALSE(matchesC4mScope(scope, Action::Subscribe, ns, std::nullopt));
  EXPECT_FALSE(matchesC4mScope(scope, Action::Subscribe, std::array<std::string, 1>{"a"}, ""));
  Grants grants{.scopes = {scope}};
  auto decoded = verifyC4mToken(signC4mToken("key", kKey, grants), kKey, kNow);
  EXPECT_TRUE(matchesC4mScope(decoded.scopes.at(0), Action::Subscribe, ns, ""));
}
