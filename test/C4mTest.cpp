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
           "a119013402",         // catreplay 2 is implementation-defined; fail closed
           "a13a0001000119012c", // moqt-reval 300
           "a1036178",           // audience requires deployment-specific matching
           "a119013701",         // CAT network restriction
           "a1636f72a0",         // composite claim
       }) {
    SCOPED_TRACE(payload);
    expectError(rawToken(payload), AuthError::UnsupportedClaim);
  }
  EXPECT_NO_THROW(verifyC4mToken(rawToken("a23a000100010019013400"), kKey, kNow));
  expectError(rawToken("a108a0"), AuthError::Malformed);     // cnf without a method
  expectError(rawToken("a119014101"), AuthError::Malformed); // catdpop is not a map
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
  // A valid namespace keeps the scope shape acceptable, so only the track rule can be rejected.
  Grants grants;
  grants.scopes.push_back(Scope{
      .actions = {Action::Publish},
      .namespaceMatches = {MatchRule{.value = "ns"}},
      .trackMatches = {MatchRule{.value = "a"}},
  });
  EXPECT_NO_THROW(signC4mToken("key", kKey, grants)); // control: one Exact track rule is accepted
  grants.scopes[0].trackMatches = {MatchRule{.type = MatchRule::Type::Contains, .value = "a"}};
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

namespace {
std::string audToken(std::vector<std::string> audiences, std::optional<std::string> issuer = {}) {
  Grants grants;
  grants.audiences = std::move(audiences);
  grants.issuer = std::move(issuer);
  return signC4mToken("key", kKey, grants);
}
} // namespace

TEST(C4mTest, AudienceMustMatchConfiguredAudience) {
  ClaimPolicy policy{.audiences = {"relay-a", "relay-b"}};
  auto grants = verifyC4mToken(audToken({"other", "relay-b"}), kKey, kNow, policy);
  EXPECT_EQ(grants.audiences.size(), 2u);
  EXPECT_NO_THROW(verifyC4mToken(audToken({"relay-a"}), kKey, kNow, policy));

  for (auto token : {audToken({"other"}), audToken({})}) {
    try {
      (void)verifyC4mToken(token, kKey, kNow, policy);
      FAIL() << "audience mismatch or absence must be rejected";
    } catch (const C4mError& e) {
      EXPECT_EQ(e.code(), AuthError::Forbidden);
    }
  }
  // Without a configured audience a token carrying aud stays rejected.
  try {
    (void)verifyC4mToken(audToken({"relay-a"}), kKey, kNow);
    FAIL() << "aud must be rejected when no audience is configured";
  } catch (const C4mError& e) {
    EXPECT_EQ(e.code(), AuthError::UnsupportedClaim);
  }
}

TEST(C4mTest, AudienceAcceptsSingleStringForm) {
  // {3: "relay-a"} per RFC 8392 (single StringOrURI).
  ClaimPolicy policy{.audiences = {"relay-a"}};
  EXPECT_NO_THROW(verifyC4mToken(rawToken("a1036772656c61792d61"), kKey, kNow, policy));
  EXPECT_THROW(verifyC4mToken(rawToken("a1036772656c61792d62"), kKey, kNow, policy), C4mError);
  // Empty array and non-text members are malformed.
  EXPECT_THROW(verifyC4mToken(rawToken("a10380"), kKey, kNow, policy), C4mError);
  EXPECT_THROW(verifyC4mToken(rawToken("a1038101"), kKey, kNow, policy), C4mError);
}

TEST(C4mTest, IssuerMustMatchConfiguredIssuer) {
  ClaimPolicy policy{.issuers = {"issuer-1"}};
  EXPECT_NO_THROW(verifyC4mToken(audToken({}, "issuer-1"), kKey, kNow, policy));
  EXPECT_THROW(verifyC4mToken(audToken({}, "issuer-2"), kKey, kNow, policy), C4mError);
  EXPECT_THROW(verifyC4mToken(audToken({}), kKey, kNow, policy), C4mError);
  // Unconfigured: iss is informational, rejected only under strict.
  EXPECT_NO_THROW(verifyC4mToken(audToken({}, "issuer-2"), kKey, kNow));
  EXPECT_THROW(verifyC4mToken(audToken({}, "issuer-2"), kKey, kNow, {.strict = true}), C4mError);
  // A validated iss is not "unvalidated informational" under strict.
  EXPECT_NO_THROW(verifyC4mToken(
      audToken({}, "issuer-1"),
      kKey,
      kNow,
      {.strict = true, .issuers = {"issuer-1"}}
  ));
}

TEST(C4mTest, ReplayProtectionRequiresCtiAndExpiration) {
  // catreplay 1 with cti and exp: accepted and flagged.
  auto grants = verifyC4mToken(
      rawToken("a3041a7fffffff0743010203190134"
               "01"),
      kKey,
      kNow
  );
  EXPECT_TRUE(grants.replayProtected);
  EXPECT_EQ(grants.cti, std::string("\x01\x02\x03", 3));
  // Missing cti or missing exp: rejected.
  expectError(rawToken("a2041a7fffffff19013401"), AuthError::Malformed);
  expectError(rawToken("a2074301020319013401"), AuthError::Malformed);
  // Round trip through the signer.
  Grants g;
  g.expiresAt = kNow + std::chrono::hours(1);
  g.cti = "abc";
  g.replayProtected = true;
  auto decoded = verifyC4mToken(signC4mToken("key", kKey, g), kKey, kNow);
  EXPECT_TRUE(decoded.replayProtected);
  EXPECT_EQ(decoded.cti, "abc");
}

// ---- DPoP (draft-02 section 4) ----

namespace {
const std::string kDraftX =
    unhex("60fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb6");
const std::string kDraftY =
    unhex("7903fe1008b8bc99a41ae9e95628bc64f2f1b20c2d7e9f5177a3c294d4462299");

std::string keyPath() {
  return std::string(TEST_C4M_KEY_DIR) + "/c4m_es256_private.pem";
}

DpopRequirement jktRequirement(std::chrono::seconds window = std::chrono::seconds(60)) {
  return DpopRequirement{
      DpopBinding{DpopBinding::Kind::Jkt, jwkThumbprint(kDraftX, kDraftY)},
      window,
      true,
  };
}
} // namespace

TEST(DpopTest, JwkThumbprintMatchesPublishedVector) {
  EXPECT_EQ(
      jwkThumbprint(kDraftX, kDraftY),
      unhex("0cebf1bc9880748a95588905b79843b42ba75cb174055e3e246bf87fe00b4a6d")
  );
}

TEST(DpopTest, TestKeyIsThePublishedKeyAndCoseThumbprintIsDeterministic) {
  const auto [x, y] = es256PublicCoordinates(keyPath());
  EXPECT_EQ(x, kDraftX);
  EXPECT_EQ(y, kDraftY);
  std::string canonical = unhex("a40102200121") + std::string("\x58\x20", 2) + kDraftX +
                          std::string("\x22\x58\x20", 3) + kDraftY;
  EXPECT_EQ(coseKeyThumbprint(kDraftX, kDraftY), sha256(canonical));
}

TEST(DpopTest, CoseKeyThumbprintMatchesDraftSection421Example) {
  EXPECT_EQ(
      coseKeyThumbprint(kDraftX, kDraftY),
      unhex("750ca40795ab23e51a75dc76fa12c052f6f6c57ba8a732337b91f096592b6ef6")
  );
}

TEST(DpopTest, TokenCarriesCnfAndCatdpop) {
  // Published vector dpop_jwk_binding: cnf {323: jkt}, catdpop {0: 60, 1: 1}.
  auto grants = verifyC4mToken(
      rawToken(
          "a401781868747470733a2f2f617574682e6578616d706c652e636f6d041a6555428008a119014358200ceb"
          "f1bc9880748a95588905b79843b42ba75cb174055e3e246bf87fe00b4a6d190141a200183c0101"
      ),
      kKey,
      kNow
  );
  ASSERT_TRUE(grants.dpop.has_value());
  EXPECT_EQ(grants.dpop->binding.kind, DpopBinding::Kind::Jkt);
  EXPECT_EQ(grants.dpop->window, std::chrono::seconds(60));
  EXPECT_TRUE(grants.dpop->detectReplay);
  // cnf with an unknown confirmation method, or catdpop without cnf, fails closed.
  expectError(rawToken("a108a11901405820" + std::string(64, '0')), AuthError::UnsupportedClaim);
  expectError(rawToken("a1190141a100183c"), AuthError::UnsupportedClaim);
}

TEST(DpopTest, ProofVerifiesForTheExactAction) {
  const std::string token = "token-bytes";
  const DpopContext context{Action::Subscribe, {"cdn", "example.com"}, std::string("/sports/live")};
  const auto proof = signDpopProof(keyPath(), token, context, "cti-1", kNow);
  const auto verified = verifyDpopProof(proof, sha256(token), jktRequirement(), context, kNow);
  EXPECT_EQ(verified.cti, "cti-1");

  auto expectRejected = [&](const DpopContext& other, std::string_view hashed = {}) {
    EXPECT_THROW(
        verifyDpopProof(
            proof,
            hashed.empty() ? sha256(token) : std::string(hashed),
            jktRequirement(),
            other,
            kNow
        ),
        C4mError
    );
  };
  expectRejected({Action::Fetch, context.trackNamespace, context.trackName});
  expectRejected({Action::Subscribe, {"cdn", "other"}, context.trackName});
  expectRejected({Action::Subscribe, context.trackNamespace, std::string("/sports/other")});
  expectRejected({Action::Subscribe, context.trackNamespace, std::nullopt});
  expectRejected(context, sha256("another token"));
}

TEST(DpopTest, ProofRejectsStaleKeyMismatchAndTampering) {
  const std::string token = "t";
  const DpopContext context{Action::ClientSetup, {}, std::nullopt};
  const auto proof = signDpopProof(keyPath(), token, context, "c", kNow);

  EXPECT_NO_THROW(verifyDpopProof(proof, sha256(token), jktRequirement(), context, kNow));
  EXPECT_NO_THROW(verifyDpopProof(
      proof,
      sha256(token),
      jktRequirement(),
      context,
      kNow + std::chrono::seconds(60)
  ));
  EXPECT_THROW(
      verifyDpopProof(
          proof,
          sha256(token),
          jktRequirement(),
          context,
          kNow + std::chrono::seconds(61)
      ),
      C4mError
  );
  EXPECT_THROW(
      verifyDpopProof(
          proof,
          sha256(token),
          jktRequirement(),
          context,
          kNow - std::chrono::seconds(61)
      ),
      C4mError
  );

  auto wrongKey = jktRequirement();
  wrongKey.binding.thumbprint = sha256("another key");
  EXPECT_THROW(verifyDpopProof(proof, sha256(token), wrongKey, context, kNow), C4mError);
  // A ckt-bound token is not satisfied by a jkt thumbprint of the same key.
  auto wrongMethod = jktRequirement();
  wrongMethod.binding.kind = DpopBinding::Kind::Ckt;
  EXPECT_THROW(verifyDpopProof(proof, sha256(token), wrongMethod, context, kNow), C4mError);
  auto ckt = jktRequirement();
  ckt.binding = DpopBinding{DpopBinding::Kind::Ckt, coseKeyThumbprint(kDraftX, kDraftY)};
  EXPECT_NO_THROW(verifyDpopProof(proof, sha256(token), ckt, context, kNow));

  for (size_t i = 0; i < proof.size(); i += 7) {
    auto tampered = proof;
    tampered[i] = static_cast<char>(tampered[i] ^ 0x01);
    EXPECT_THROW(
        verifyDpopProof(tampered, sha256(token), jktRequirement(), context, kNow),
        C4mError
    ) << "byte "
      << i;
  }
  EXPECT_THROW(verifyDpopProof("", sha256(token), jktRequirement(), context, kNow), C4mError);
  EXPECT_THROW(
      verifyDpopProof(
          proof.substr(0, proof.size() - 1),
          sha256(token),
          jktRequirement(),
          context,
          kNow
      ),
      C4mError
  );
}

// ---- moqt-reval (draft-02 section 2.2) ----

TEST(C4mTest, RevalidationIntervalHonoredOnlyWhenAtLeastTheConfiguredMinimum) {
  // -65538 (moqt-reval): 300 seconds, with exp.
  const auto token = rawToken("a23a0001000119012c041a7fffffff");
  ClaimPolicy policy{.minRevalidation = std::chrono::seconds(5)};
  auto grants = verifyC4mToken(token, kKey, kNow, policy);
  EXPECT_EQ(grants.revalidateEvery, std::chrono::seconds(300));

  // Shorter than the relay can revalidate, or revalidation disabled: rejected (section 2.2).
  policy.minRevalidation = std::chrono::seconds(301);
  EXPECT_THROW(verifyC4mToken(token, kKey, kNow, policy), C4mError);
  EXPECT_THROW(verifyC4mToken(token, kKey, kNow), C4mError);
  // Zero means never revalidate and is always accepted.
  const auto zero = rawToken("a23a0001000100041a7fffffff");
  EXPECT_EQ(verifyC4mToken(zero, kKey, kNow).revalidateEvery, std::chrono::seconds(0));
  // A non-integer interval is malformed.
  EXPECT_THROW(
      verifyC4mToken(
          rawToken("a13a000100016161"),
          kKey,
          kNow,
          {.minRevalidation = std::chrono::seconds(5)}
      ),
      C4mError
  );
}

TEST(C4mTest, RevalidationIntervalRoundTripsThroughTheSigner) {
  Grants g;
  g.expiresAt = kNow + std::chrono::hours(1);
  g.revalidateEvery = std::chrono::seconds(60);
  auto decoded = verifyC4mToken(
      signC4mToken("key", kKey, g),
      kKey,
      kNow,
      ClaimPolicy{.minRevalidation = std::chrono::seconds(1)}
  );
  EXPECT_EQ(decoded.revalidateEvery, std::chrono::seconds(60));
}

// ---- ES384 and EdDSA proofs (draft-02 section 4.2: SHOULD) ----

namespace {
std::string keyFile(const char* name) {
  return std::string(TEST_C4M_KEY_DIR) + "/" + name;
}

struct ProofKeyCase {
  const char* file;
  int64_t algorithm;
};

const ProofKeyCase kProofKeys[] = {
    {"c4m_es256_private.pem", kDpopAlgEs256},
    {"c4m_es384_private.pem", kDpopAlgEs384},
    {"c4m_ed25519_private.pem", kDpopAlgEdDsa},
};

DpopRequirement bindingFor(const DpopKey& key, DpopBinding::Kind kind) {
  return DpopRequirement{
      DpopBinding{
          kind,
          kind == DpopBinding::Kind::Ckt ? coseKeyThumbprint(key) : jwkThumbprint(key)
      },
      std::chrono::seconds(60),
      true,
  };
}
} // namespace

TEST(DpopTest, ThumbprintsMatchTheRfcVectors) {
  // RFC 9679 section 6: COSE Key Thumbprint of an EC2 P-256 key.
  EXPECT_EQ(
      coseKeyThumbprint(
          unhex("65eda5a12577c2bae829437fe338701a10aaa375e1bb5b5de108de439c08551d"),
          unhex("1e52ed75701163f7f9e40ddf9f341b3dc9ba860af7e0ca7ca7e9eecd0084d19c")
      ),
      unhex("496bd8afadf307e5b08c64b0421bf9dc01528a344a43bda88fadd1669da253ec")
  );
  // RFC 8037 appendix A.3: JWK Thumbprint of an Ed25519 key (x as base64url
  // 11qYAYKxCrfVS_7TyWQHOg7hcvPapiMlrwIaaPcHURo, thumbprint
  // kPrK_qmxVWaYVA9wwBF6Iuo3vVzz7TxHCTwXBygrS4k).
  DpopKey ed{
      kDpopAlgEdDsa,
      unhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"),
      {}
  };
  EXPECT_EQ(
      jwkThumbprint(ed),
      unhex("90facafea9b1556698540f70c0117a22ea37bd5cf3ed3c47093c1707282b4b89")
  );
}

TEST(DpopTest, KeysAreReadFromPemWithTheRightAlgorithm) {
  for (const auto& c : kProofKeys) {
    SCOPED_TRACE(c.file);
    const auto key = dpopPublicKey(keyFile(c.file));
    EXPECT_EQ(key.algorithm, c.algorithm);
    EXPECT_EQ(key.x.size(), c.algorithm == kDpopAlgEs384 ? 48u : 32u);
    EXPECT_EQ(
        key.y.size(),
        c.algorithm == kDpopAlgEdDsa ? 0u : (c.algorithm == kDpopAlgEs384 ? 48u : 32u)
    );
  }
}

TEST(DpopTest, ProofsVerifyForEveryAlgorithmAndBindingMethod) {
  const std::string token = "token-bytes";
  const DpopContext context{Action::Subscribe, {"cdn", "example.com"}, std::string("/sports/live")};
  for (const auto& c : kProofKeys) {
    SCOPED_TRACE(c.file);
    const auto key = dpopPublicKey(keyFile(c.file));
    const auto proof = signDpopProof(keyFile(c.file), token, context, "cti-1", kNow);
    for (auto kind : {DpopBinding::Kind::Ckt, DpopBinding::Kind::Jkt}) {
      const auto verified =
          verifyDpopProof(proof, sha256(token), bindingFor(key, kind), context, kNow);
      EXPECT_EQ(verified.cti, "cti-1");
    }
    const auto binding = bindingFor(key, DpopBinding::Kind::Ckt);
    EXPECT_THROW(
        verifyDpopProof(
            proof,
            sha256(token),
            binding,
            {Action::Fetch, context.trackNamespace, context.trackName},
            kNow
        ),
        C4mError
    );
    EXPECT_THROW(verifyDpopProof(proof, sha256("other"), binding, context, kNow), C4mError);
    EXPECT_THROW(
        verifyDpopProof(proof, sha256(token), binding, context, kNow + std::chrono::seconds(61)),
        C4mError
    );
    for (size_t i = 0; i < proof.size(); i += 5) {
      auto tampered = proof;
      tampered[i] = static_cast<char>(tampered[i] ^ 0x01);
      EXPECT_THROW(verifyDpopProof(tampered, sha256(token), binding, context, kNow), C4mError)
          << "byte " << i;
    }
  }
}

TEST(DpopTest, AProofIsOnlyAcceptedForTheKeyTheTokenIsBoundTo) {
  const std::string token = "t";
  const DpopContext context{Action::ClientSetup, {}, std::nullopt};
  for (const auto& signer : kProofKeys) {
    const auto proof = signDpopProof(keyFile(signer.file), token, context, "c", kNow);
    for (const auto& bound : kProofKeys) {
      const auto binding = bindingFor(dpopPublicKey(keyFile(bound.file)), DpopBinding::Kind::Ckt);
      if (signer.algorithm == bound.algorithm) {
        EXPECT_NO_THROW(verifyDpopProof(proof, sha256(token), binding, context, kNow));
      } else {
        EXPECT_THROW(verifyDpopProof(proof, sha256(token), binding, context, kNow), C4mError)
            << signer.file << " proof against " << bound.file << " binding";
      }
    }
  }
}

TEST(DpopTest, UnsupportedProofAlgorithmsAreRejected) {
  const std::string token = "t";
  const DpopContext context{Action::ClientSetup, {}, std::nullopt};
  const auto key = dpopPublicKey(keyFile("c4m_es384_private.pem"));
  auto proof = signDpopProof(keyFile("c4m_es384_private.pem"), token, context, "c", kNow);
  // alg -35 is "38 22"; -36 ("38 23") is not a supported algorithm.
  const auto at = proof.find("\x01\x38\x22");
  ASSERT_NE(at, std::string::npos);
  proof[at + 2] = '\x23';
  EXPECT_THROW(
      verifyDpopProof(proof, sha256(token), bindingFor(key, DpopBinding::Kind::Ckt), context, kNow),
      C4mError
  );
}
