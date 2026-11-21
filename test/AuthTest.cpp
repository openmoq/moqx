/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "auth/Auth.h"
#include "auth/AuthTokenIssuer.h"
#include "auth/Dpop.h"
#include "auth/Es256Key.h"
#include "auth/HmacKey.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <array>
#include <folly/String.h>
#include <folly/portability/GTest.h>
#include <future>

using namespace openmoq::moqx;
using namespace openmoq::moqx::auth;
using namespace moxygen;

namespace {

config::AuthConfig makeConfig() {
  return config::AuthConfig{
      .enabled = true,
      .tokenType = 77,
      .hmacKeys = {config::AuthConfig::HmacKey{.id = "k1", .secret = "secret"}},
      .requireSetupToken = true,
      .allowRequestTokenOverride = true,
      .maxTokensPerMessage = 4,
  };
}

Grants makeGrants(
    std::vector<Action> actions,
    std::vector<MatchRule> namespaceMatches,
    std::vector<MatchRule> trackMatches
) {
  Grants grants;
  grants.expiresAt = std::chrono::system_clock::now() + std::chrono::hours(1);
  grants.scopes.push_back(Scope{
      .actions = std::move(actions),
      .namespaceMatches = std::move(namespaceMatches),
      .trackMatches = std::move(trackMatches),
  });
  return grants;
}

AuthToken
makeToken(Grants grants, std::string_view secret = "secret", std::string_view keyID = "k1") {
  return AuthToken{
      .tokenType = 77,
      .tokenValue = signGrants(keyID, secret, grants),
      .alias = AuthToken::DontRegister,
  };
}

std::vector<uint8_t> cborBytes(const std::vector<uint8_t>& data) {
  std::vector<uint8_t> out;
  if (data.size() < 24) {
    out.push_back(static_cast<uint8_t>(0x40 | data.size()));
  } else {
    out.push_back(0x58);
    out.push_back(static_cast<uint8_t>(data.size()));
  }
  out.insert(out.end(), data.begin(), data.end());
  return out;
}

// Builds a COSE_Mac0 token from a hex claims map, so that a test can send any claim value.
AuthToken makeRawCwt(std::string_view claimsHex) {
  const std::vector<uint8_t> header{0xa1, 0x01, static_cast<uint8_t>(5)};
  const auto claims = folly::unhexlify(claimsHex);
  const std::vector<uint8_t> payload(claims.begin(), claims.end());
  const auto key = deriveHmacKey("secret");
  std::vector<uint8_t> input{0x84, 0x64, 'M', 'A', 'C', '0'};
  for (const auto& part : {cborBytes(header), std::vector<uint8_t>{0x40}, cborBytes(payload)}) {
    input.insert(input.end(), part.begin(), part.end());
  }
  std::vector<uint8_t> tag(32);
  unsigned tagLen = 0;
  HMAC(
      EVP_sha256(),
      key.data(),
      static_cast<int>(key.size()),
      input.data(),
      input.size(),
      tag.data(),
      &tagLen
  );
  std::vector<uint8_t> cwt{0xd1, 0x84};
  for (const auto& part :
       {cborBytes(header), std::vector<uint8_t>{0xa0}, cborBytes(payload), cborBytes(tag)}) {
    cwt.insert(cwt.end(), part.begin(), part.end());
  }
  return AuthToken{
      .tokenType = 77,
      .tokenValue = std::string(cwt.begin(), cwt.end()),
      .alias = AuthToken::DontRegister,
  };
}

folly::Expected<Grants, AuthError> verifyStrict(const AuthToken& token) {
  auto config = makeConfig();
  config.strictClaims = true;
  return AuthTokenVerifier(config).verify(token);
}

config::AuthConfig::AnonymousScope makeAnonymousConfigScope(
    std::vector<Action> actions,
    std::optional<std::vector<std::string>> namespaceSegments = std::nullopt,
    std::optional<std::string> trackName = std::nullopt
) {
  return config::AuthConfig::AnonymousScope{
      .actions = std::move(actions),
      .namespaceSegments = std::move(namespaceSegments),
      .trackName = std::move(trackName),
  };
}

} // namespace

TEST(AuthTest, MixedAlgorithmKeysRejectAlgorithmConfusionAndTryNoKidWithinFamily) {
  auto config = makeConfig();
  config.es256Keys = {
      {.id = "es-key", .publicKeyFile = std::string(TEST_C4M_KEY_DIR) + "/c4m_es256_public.pem"}
  };
  AuthTokenVerifier verifier(config);
  auto privateKey =
      loadEs256PrivateKeyFile(std::string(TEST_C4M_KEY_DIR) + "/c4m_es256_private.pem");
  auto grants = makeGrants({Action::ClientSetup}, {}, {});
  auto token = AuthToken{
      .tokenType = 77,
      .tokenValue = signC4mToken("es-key", *privateKey, grants),
      .alias = AuthToken::DontRegister
  };
  EXPECT_TRUE(verifier.verify(token).hasValue());
  token.tokenValue = signC4mToken("", *privateKey, grants);
  EXPECT_TRUE(verifier.verify(token).hasValue());
  token.tokenValue = signC4mToken("k1", *privateKey, grants);
  auto confused = verifier.verify(token);
  ASSERT_TRUE(confused.hasError());
  EXPECT_EQ(confused.error(), AuthError::BadSignature);
  token.tokenValue = signGrants("es-key", "secret", grants);
  confused = verifier.verify(token);
  ASSERT_TRUE(confused.hasError());
  EXPECT_EQ(confused.error(), AuthError::BadSignature);
}

TEST(AuthTest, DisabledAuthDoesNotLoadEs256FilesAndDuplicateKeyIdsFailClosed) {
  auto config = makeConfig();
  config.enabled = false;
  config.es256Keys = {{.id = "es-key", .publicKeyFile = "missing.pem"}};
  EXPECT_NO_THROW(AuthTokenVerifier{config});
  config.enabled = true;
  config.es256Keys = {
      {.id = "k1", .publicKeyFile = std::string(TEST_C4M_KEY_DIR) + "/c4m_es256_public.pem"}
  };
  EXPECT_THROW(AuthTokenVerifier{config}, std::invalid_argument);
}

TEST(AuthTest, Es256VerifierCanBeSharedAcrossThreads) {
  auto config = makeConfig();
  config.hmacKeys.clear();
  config.es256Keys = {
      {.id = "es-key", .publicKeyFile = std::string(TEST_C4M_KEY_DIR) + "/c4m_es256_public.pem"}
  };
  const AuthTokenVerifier verifier(config);
  const auto privateKey =
      loadEs256PrivateKeyFile(std::string(TEST_C4M_KEY_DIR) + "/c4m_es256_private.pem");
  const auto token = AuthToken{
      .tokenType = 77,
      .tokenValue = signC4mToken("es-key", *privateKey, makeGrants({Action::ClientSetup}, {}, {})),
      .alias = AuthToken::DontRegister
  };
  std::vector<std::future<bool>> workers;
  for (size_t i = 0; i < 8; ++i) {
    workers.push_back(std::async(std::launch::async, [&] {
      for (size_t n = 0; n < 64; ++n) {
        const auto result = verifier.verify(token);
        if (!result.hasValue() || !allows(result.value(), Action::ClientSetup, TrackNamespace{})) {
          return false;
        }
      }
      return true;
    }));
  }
  for (auto& worker : workers) {
    EXPECT_TRUE(worker.get());
  }
}

TEST(AuthTest, VerifiesSignedTokenAndAllowsMatchingAction) {
  TrackNamespace ns{{"live", "event"}};
  AuthTokenVerifier verifier(makeConfig());
  auto token = makeToken(makeGrants(
      {Action::Subscribe},
      {MatchRule{.value = "live"}, MatchRule{.value = "event"}},
      {MatchRule{.type = MatchRule::Type::Exact, .value = "video"}}
  ));
  auto grants = verifier.verify(token);
  ASSERT_TRUE(grants.hasValue());
  EXPECT_TRUE(allows(grants.value(), Action::Subscribe, FullTrackName{ns, "video"}));
  EXPECT_FALSE(allows(grants.value(), Action::Publish, FullTrackName{ns, "video"}));
  EXPECT_FALSE(allows(grants.value(), Action::Subscribe, FullTrackName{ns, "audio"}));
}

TEST(AuthTest, AllowsPerFieldPrefixAndSuffixMatchRules) {
  TrackNamespace ns{{"live", "event"}};
  auto grants = makeGrants(
      {Action::Subscribe},
      {MatchRule{.type = MatchRule::Type::Prefix, .value = "liv"},
       MatchRule{.type = MatchRule::Type::Suffix, .value = "vent"}},
      {MatchRule{.type = MatchRule::Type::Prefix, .value = "vid"}}
  );
  EXPECT_TRUE(allows(grants, Action::Subscribe, FullTrackName{ns, "video"}));
  EXPECT_FALSE(
      allows(grants, Action::Subscribe, FullTrackName{TrackNamespace{{"vod", "event"}}, "video"})
  );
  EXPECT_FALSE(
      allows(grants, Action::Subscribe, FullTrackName{TrackNamespace{{"live", "other"}}, "video"})
  );
  EXPECT_FALSE(allows(grants, Action::Subscribe, FullTrackName{ns, "audio"}));
}

TEST(AuthTest, EmptyNamespaceAndTrackRulesMatchEverything) {
  auto grants = makeGrants({Action::Subscribe}, {}, {});

  EXPECT_TRUE(allows(grants, Action::Subscribe, FullTrackName{TrackNamespace{{"live"}}, "video"}));
  EXPECT_TRUE(allows(grants, Action::Subscribe, TrackNamespace{}));
}

TEST(AuthTest, AllowsRejectsEmptyScopesAndExpiredGrants) {
  Grants emptyScopes;
  emptyScopes.expiresAt = std::chrono::system_clock::now() + std::chrono::hours(1);
  EXPECT_FALSE(
      allows(emptyScopes, Action::Subscribe, FullTrackName{TrackNamespace{{"live"}}, "video"})
  );

  auto grants = makeGrants({Action::Subscribe}, {}, {});
  const auto now = std::chrono::system_clock::now();
  grants.expiresAt = now - std::chrono::seconds(1);
  EXPECT_FALSE(
      allows(grants, Action::Subscribe, FullTrackName{TrackNamespace{{"live"}}, "video"}, now)
  );
}

TEST(AuthTest, AllowsAnyPermitsWhenAnyElementAllows) {
  TrackNamespace ns{{"live"}};
  std::vector<Grants> grantsList{
      makeGrants({Action::Publish}, {}, {}),
      makeGrants({Action::Subscribe}, {}, {}),
  };
  EXPECT_TRUE(allowsAny(grantsList, Action::Subscribe, ns));
  EXPECT_TRUE(allowsAny(grantsList, Action::Subscribe, FullTrackName{ns, "video"}));
  EXPECT_FALSE(allowsAny(grantsList, Action::Fetch, ns));
}

TEST(AuthTest, AllowsAnyDeniesWhenNoElementAllows) {
  std::vector<Grants> grantsList{makeGrants({Action::Publish}, {}, {})};
  TrackNamespace ns{{"live"}};
  EXPECT_FALSE(allowsAny(grantsList, Action::Subscribe, ns));
  EXPECT_FALSE(allowsAny(grantsList, Action::Subscribe, FullTrackName{ns, "video"}));
}

TEST(AuthTest, AllowsAnyOnEmptyVectorIsFalse) {
  std::vector<Grants> empty;
  TrackNamespace ns{{"live"}};
  EXPECT_FALSE(allowsAny(empty, Action::Subscribe, ns));
  EXPECT_FALSE(allowsAny(empty, Action::Subscribe, FullTrackName{ns, "video"}));
}

TEST(AuthTest, FindAuthTokensSelectsMatchingAuthorizationToken) {
  Parameters params(FrameType::SUBSCRIBE);
  ASSERT_TRUE(
      params
          .insertParam(Parameter(
              static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN),
              AuthToken{.tokenType = 76, .tokenValue = "wrong", .alias = AuthToken::DontRegister}
          ))
          .hasValue()
  );
  ASSERT_TRUE(
      params
          .insertParam(Parameter(
              static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN),
              AuthToken{.tokenType = 77, .tokenValue = "right", .alias = AuthToken::DontRegister}
          ))
          .hasValue()
  );

  auto tokens = findAuthTokens(params, 77);
  ASSERT_EQ(tokens.size(), 1);
  EXPECT_EQ(tokens[0].tokenValue, "right");
  EXPECT_TRUE(findAuthTokens(params, 78).empty());
}

TEST(AuthTest, FindAuthTokensReturnsAllMatchingTokens) {
  Parameters params(FrameType::SUBSCRIBE);
  ASSERT_TRUE(
      params
          .insertParam(Parameter(
              static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN),
              AuthToken{.tokenType = 77, .tokenValue = "first", .alias = AuthToken::DontRegister}
          ))
          .hasValue()
  );
  ASSERT_TRUE(params
                  .insertParam(Parameter(
                      static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN),
                      AuthToken{
                          .tokenType = 76,
                          .tokenValue = "other-type",
                          .alias = AuthToken::DontRegister
                      }
                  ))
                  .hasValue());
  ASSERT_TRUE(
      params
          .insertParam(Parameter(
              static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN),
              AuthToken{.tokenType = 77, .tokenValue = "second", .alias = AuthToken::DontRegister}
          ))
          .hasValue()
  );

  auto tokens = findAuthTokens(params, 77);
  ASSERT_EQ(tokens.size(), 2);
  EXPECT_EQ(tokens[0].tokenValue, "first");
  EXPECT_EQ(tokens[1].tokenValue, "second");
}

TEST(AuthTest, RejectsBadSignature) {
  auto token = makeToken(makeGrants({Action::ClientSetup}, {}, {}));
  token.tokenValue.back() ^= 0x01;
  AuthTokenVerifier verifier(makeConfig());
  auto grants = verifier.verify(token);
  ASSERT_TRUE(grants.hasError());
  EXPECT_EQ(grants.error(), AuthError::BadSignature);
}

TEST(AuthTest, SelectsConfiguredKeyByID) {
  auto config = makeConfig();
  config.hmacKeys.push_back(config::AuthConfig::HmacKey{.id = "k2", .secret = "secret-2"});

  AuthTokenVerifier verifier(config);
  auto grants =
      verifier.verify(makeToken(makeGrants({Action::ClientSetup}, {}, {}), "secret-2", "k2"));
  ASSERT_TRUE(grants.hasValue());

  auto missingKey =
      verifier.verify(makeToken(makeGrants({Action::ClientSetup}, {}, {}), "secret-3", "k3"));
  ASSERT_TRUE(missingKey.hasError());
  EXPECT_EQ(missingKey.error(), AuthError::BadSignature);
}

TEST(AuthTest, RejectsWrongTokenType) {
  auto token = makeToken(makeGrants({Action::ClientSetup}, {}, {}));
  token.tokenType = 78;
  AuthTokenVerifier verifier(makeConfig());

  auto grants = verifier.verify(token);
  ASSERT_TRUE(grants.hasError());
  EXPECT_EQ(grants.error(), AuthError::WrongTokenType);
}

TEST(AuthTest, RejectsEmptyTokenAsMalformed) {
  AuthTokenVerifier verifier(makeConfig());

  auto empty = makeToken(makeGrants({Action::ClientSetup}, {}, {}));
  empty.tokenValue.clear();
  auto emptyRes = verifier.verify(empty);
  ASSERT_TRUE(emptyRes.hasError());
  EXPECT_EQ(emptyRes.error(), AuthError::Malformed);
}

// Non-empty garbage that isn't a valid COSE/CWT structure must be rejected
// cleanly (no crash). Either Malformed (CBOR/COSE decode fails) or BadSignature
// (decodes but no key validates) is acceptable.
TEST(AuthTest, RejectsGarbageBytesAsMalformedOrBadSig) {
  AuthTokenVerifier verifier(makeConfig());
  AuthToken token{
      .tokenType = 77,
      .tokenValue = std::string("\xde\xad\xbe\xef", 4),
      .alias = AuthToken::DontRegister,
  };
  auto result = verifier.verify(token);
  ASSERT_TRUE(result.hasError());
  EXPECT_TRUE(result.error() == AuthError::Malformed || result.error() == AuthError::BadSignature);
}

TEST(AuthTest, RejectsExpiredToken) {
  auto expired = makeGrants({Action::ClientSetup}, {}, {});
  expired.expiresAt = std::chrono::system_clock::time_point(std::chrono::seconds(1'735'689'600));
  AuthTokenVerifier verifier(makeConfig());
  auto grants = verifier.verify(makeToken(expired));
  ASSERT_TRUE(grants.hasError());
  EXPECT_EQ(grants.error(), AuthError::Expired);
}

TEST(AuthTest, VerifiesC4mCwtWithOpenScope) {
  AuthTokenVerifier verifier(makeConfig());
  auto grants = verifier.verify(makeToken(makeGrants({Action::ClientSetup}, {}, {})));

  ASSERT_TRUE(grants.hasValue());
  EXPECT_TRUE(allows(grants.value(), Action::ClientSetup, TrackNamespace{}));
}

TEST(AuthTest, StrictClaimsAcceptsOnlyEnforcedClaims) {
  auto config = makeConfig();
  config.strictClaims = true;
  AuthTokenVerifier verifier(config);
  EXPECT_TRUE(verifier.verify(makeToken(makeGrants({Action::ClientSetup}, {}, {}))).hasValue());
}

TEST(AuthTest, StrictClaimsRejectsUnenforcedClaim) {
  auto token = makeRawCwt("a2041af4865700016178"); // {exp: 4102444800, iss: "x"}

  EXPECT_TRUE(AuthTokenVerifier(makeConfig()).verify(token).hasValue());

  auto config = makeConfig();
  config.strictClaims = true;
  auto strict = AuthTokenVerifier(config).verify(token);
  ASSERT_TRUE(strict.hasError());
  EXPECT_EQ(strict.error(), AuthError::UnsupportedClaim);
}

TEST(AuthTest, StrictClaimsAcceptsRawCwtWithoutKid) {
  auto token = makeRawCwt("a1041af4865700"); // {exp: 4102444800}
  EXPECT_TRUE(AuthTokenVerifier(makeConfig()).verify(token).hasValue());
  EXPECT_TRUE(verifyStrict(token).hasValue());
}

TEST(AuthTest, StrictClaimsRejectsMalformedExp) {
  for (auto claims : {
           "a1046a31373030303030303030", // {exp: "1700000000"}
           "a2041af4865700041af4865700", // exp twice
       }) {
    SCOPED_TRACE(claims);
    auto strict = verifyStrict(makeRawCwt(claims));
    ASSERT_TRUE(strict.hasError());
    EXPECT_EQ(strict.error(), AuthError::Malformed);
  }
}

TEST(AuthTest, MoqtRevalIsHonoredAtOrAboveTheConfiguredMinimum) {
  // {exp, moqt-reval: 60}: a restriction the relay enforces, not an informational claim.
  const auto token = makeRawCwt("a2041af48657003a00010001183c");
  auto accepted = verifyStrict(token);
  ASSERT_TRUE(accepted.hasValue());
  EXPECT_EQ(accepted.value().revalidateEvery, std::chrono::seconds(60));

  auto config = makeConfig();
  config.minRevalidationSeconds = 61; // the relay cannot revalidate that often
  auto tooFrequent = AuthTokenVerifier(config).verify(token);
  ASSERT_TRUE(tooFrequent.hasError());
  EXPECT_EQ(tooFrequent.error(), AuthError::UnsupportedClaim);
}

TEST(AuthTest, MoqtRevalIsRejectedWhenRevalidationIsDisabled) {
  auto config = makeConfig();
  config.minRevalidationSeconds = 0; // draft-02 2.2: unable to revalidate, so reject all
  auto rejected = AuthTokenVerifier(config).verify(makeRawCwt("a2041af48657003a00010001183c"));
  ASSERT_TRUE(rejected.hasError());
  EXPECT_EQ(rejected.error(), AuthError::UnsupportedClaim);
}

TEST(AuthTest, StrictClaimsReportsExpiryFirst) {
  auto strict = verifyStrict(makeRawCwt("a20401016178")); // {exp: 1, iss: "x"}
  ASSERT_TRUE(strict.hasError());
  EXPECT_EQ(strict.error(), AuthError::Expired);
}

TEST(AuthTest, RejectsCompoundTrackMatchesThatC4mCannotRepresent) {
  auto grants = makeGrants(
      {Action::Subscribe},
      {},
      {MatchRule{.type = MatchRule::Type::Prefix, .value = "live-"},
       MatchRule{.type = MatchRule::Type::Suffix, .value = ".mp4"}}
  );
  EXPECT_THROW(makeToken(grants), std::invalid_argument);
}

// --- authorize(): multi-token, any-satisfies pooling ---

namespace {

Parameters withAuthToken(FrameType frameType, AuthToken token) {
  Parameters params(frameType);
  auto ok = params.insertParam(
      Parameter(static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN), std::move(token))
  );
  EXPECT_TRUE(ok.hasValue());
  return params;
}

} // namespace

TEST(AuthTest, AuthorizeRequestTokenGrantsWhenSessionGrantsDoNot) {
  TrackNamespace ns{{"live"}};
  AuthTokenVerifier verifier(makeConfig());
  auto params =
      withAuthToken(FrameType::SUBSCRIBE, makeToken(makeGrants({Action::Subscribe}, {}, {})));

  std::vector<Grants> sessionGrants; // empty: session alone doesn't cover Subscribe
  auto result = authorize(verifier, Action::Subscribe, params, ns, sessionGrants, "video");
  EXPECT_TRUE(result.hasValue());
}

TEST(AuthTest, AuthorizeSessionGrantsGrantWhenNoRequestToken) {
  TrackNamespace ns{{"live"}};
  AuthTokenVerifier verifier(makeConfig());
  Parameters params(FrameType::SUBSCRIBE); // no request-level token
  std::vector<Grants> sessionGrants{makeGrants({Action::Subscribe}, {}, {})};

  auto result = authorize(verifier, Action::Subscribe, params, ns, sessionGrants, "video");
  EXPECT_TRUE(result.hasValue());
}

TEST(AuthTest, AuthorizeGarbageRequestTokenDoesNotBlockSessionGrants) {
  TrackNamespace ns{{"live"}};
  AuthTokenVerifier verifier(makeConfig());
  auto params = withAuthToken(
      FrameType::SUBSCRIBE,
      AuthToken{
          .tokenType = 77,
          .tokenValue = std::string("\xde\xad\xbe\xef", 4),
          .alias = AuthToken::DontRegister
      }
  );
  std::vector<Grants> sessionGrants{makeGrants({Action::Subscribe}, {}, {})};

  auto result = authorize(verifier, Action::Subscribe, params, ns, sessionGrants, "video");
  EXPECT_TRUE(result.hasValue());
}

TEST(AuthTest, AuthorizeReturnsMostSpecificErrorWhenNothingPermits) {
  TrackNamespace ns{{"live"}};
  AuthTokenVerifier verifier(makeConfig());
  auto badToken = makeToken(makeGrants({Action::Subscribe}, {}, {}));
  badToken.tokenValue.back() ^= 0x01; // corrupt signature
  auto params = withAuthToken(FrameType::SUBSCRIBE, std::move(badToken));
  std::vector<Grants> sessionGrants; // doesn't cover the action either

  auto result = authorize(verifier, Action::Subscribe, params, ns, sessionGrants, "video");
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error(), AuthError::BadSignature);
}

// Regression: a corrupted request token must not eclipse a sibling token that
// verified fine but simply lacked scope -- the latter makes Forbidden the
// correct error, not the former's BadSignature.
TEST(AuthTest, AuthorizePrefersForbiddenOverBadSignature) {
  TrackNamespace ns{{"live"}};
  AuthTokenVerifier verifier(makeConfig());
  Parameters params(FrameType::SUBSCRIBE);
  auto badToken = makeToken(makeGrants({Action::Subscribe}, {}, {}));
  badToken.tokenValue.back() ^= 0x01; // corrupt signature
  ASSERT_TRUE(
      params
          .insertParam(
              Parameter(static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN), badToken)
          )
          .hasValue()
  );
  ASSERT_TRUE(params
                  .insertParam(Parameter(
                      static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN),
                      makeToken(makeGrants({Action::Publish}, {}, {})) // verifies, but wrong action
                  ))
                  .hasValue());
  std::vector<Grants> sessionGrants; // doesn't cover the action either

  auto result = authorize(verifier, Action::Subscribe, params, ns, sessionGrants, "video");
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error(), AuthError::Forbidden);
}

TEST(AuthTest, AuthorizeReturnsForbiddenWhenNothingCoversAction) {
  TrackNamespace ns{{"live"}};
  AuthTokenVerifier verifier(makeConfig());
  Parameters params(FrameType::SUBSCRIBE);                                  // no request token
  std::vector<Grants> sessionGrants{makeGrants({Action::Publish}, {}, {})}; // different action

  auto result = authorize(verifier, Action::Subscribe, params, ns, sessionGrants, "video");
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error(), AuthError::Forbidden);
}

// --- authenticateSetup(): multi-token, any-satisfies pooling ---

TEST(AuthTest, AuthenticateSetupAcceptsAnyOfMultipleSetupTokens) {
  AuthTokenVerifier verifier(makeConfig());
  auto badToken = makeToken(makeGrants({Action::ClientSetup}, {}, {}));
  badToken.tokenValue.back() ^= 0x01; // corrupt signature

  Parameters params(FrameType::CLIENT_SETUP);
  ASSERT_TRUE(
      params
          .insertParam(
              Parameter(static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN), badToken)
          )
          .hasValue()
  );
  ASSERT_TRUE(params
                  .insertParam(Parameter(
                      static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN),
                      makeToken(makeGrants({Action::ClientSetup}, {}, {}))
                  ))
                  .hasValue());

  auto result = authenticateSetup(verifier, params);
  ASSERT_TRUE(result.hasValue());
  ASSERT_TRUE(result.value());
  // Only the token that actually verified pools; the corrupted one is dropped.
  EXPECT_EQ(result.value()->size(), 1u);
}

TEST(AuthTest, AuthenticateSetupPoolsAllVerifiedTokensRegardlessOfWhichGrantsClientSetup) {
  AuthTokenVerifier verifier(makeConfig());
  Parameters params(FrameType::CLIENT_SETUP);
  ASSERT_TRUE(params
                  .insertParam(Parameter(
                      static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN),
                      makeToken(makeGrants({Action::ClientSetup}, {}, {}))
                  ))
                  .hasValue());
  ASSERT_TRUE(params
                  .insertParam(Parameter(
                      static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN),
                      makeToken(makeGrants({Action::Subscribe}, {}, {}))
                  ))
                  .hasValue());

  auto result = authenticateSetup(verifier, params);
  ASSERT_TRUE(result.hasValue());
  ASSERT_TRUE(result.value());
  ASSERT_EQ(result.value()->size(), 2u);
  EXPECT_TRUE(allowsAny(*result.value(), Action::Subscribe, TrackNamespace{}));
}

TEST(AuthTest, AuthenticateSetupRejectsWhenNoTokenGrantsClientSetup) {
  AuthTokenVerifier verifier(makeConfig());
  Parameters params(FrameType::CLIENT_SETUP);
  ASSERT_TRUE(params
                  .insertParam(Parameter(
                      static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN),
                      makeToken(makeGrants({Action::Subscribe}, {}, {}))
                  ))
                  .hasValue());

  auto result = authenticateSetup(verifier, params);
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error(), AuthError::Forbidden);
}

// Regression: a corrupted setup token must not eclipse a sibling token that
// verified fine but simply didn't grant ClientSetup -- the latter makes
// Forbidden the correct error, not the former's BadSignature.
TEST(AuthTest, AuthenticateSetupPrefersForbiddenOverBadSignature) {
  AuthTokenVerifier verifier(makeConfig());
  auto badToken = makeToken(makeGrants({Action::ClientSetup}, {}, {}));
  badToken.tokenValue.back() ^= 0x01; // corrupt signature

  Parameters params(FrameType::CLIENT_SETUP);
  ASSERT_TRUE(
      params
          .insertParam(
              Parameter(static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN), badToken)
          )
          .hasValue()
  );
  ASSERT_TRUE(
      params
          .insertParam(Parameter(
              static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN),
              makeToken(makeGrants({Action::Subscribe}, {}, {})) // verifies, but not ClientSetup
          ))
          .hasValue()
  );

  auto result = authenticateSetup(verifier, params);
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error(), AuthError::Forbidden);
}

TEST(AuthTest, AuthenticateSetupSurfacesMostSpecificErrorWhenNothingGrantsClientSetup) {
  AuthTokenVerifier verifier(makeConfig());
  auto badToken = makeToken(makeGrants({Action::ClientSetup}, {}, {}));
  badToken.tokenValue.back() ^= 0x01; // corrupt signature

  Parameters params(FrameType::CLIENT_SETUP);
  ASSERT_TRUE(
      params
          .insertParam(
              Parameter(static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN), badToken)
          )
          .hasValue()
  );

  auto result = authenticateSetup(verifier, params);
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error(), AuthError::BadSignature);
}

// Regression: require_setup_token=false must never fail the connection over
// the setup token, even when one is presented and doesn't grant ClientSetup.
TEST(AuthTest, AuthenticateSetupNotRequiredPoolsGrantsWithoutClientSetup) {
  auto config = makeConfig();
  config.requireSetupToken = false;
  AuthTokenVerifier verifier(config);

  Parameters params(FrameType::CLIENT_SETUP);
  ASSERT_TRUE(
      params
          .insertParam(Parameter(
              static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN),
              makeToken(makeGrants({Action::Subscribe}, {}, {})) // verifies, but not ClientSetup
          ))
          .hasValue()
  );

  auto result = authenticateSetup(verifier, params);
  ASSERT_TRUE(result.hasValue());
  ASSERT_TRUE(result.value());
  ASSERT_EQ(result.value()->size(), 1u);
  EXPECT_TRUE(allowsAny(*result.value(), Action::Subscribe, TrackNamespace{}));
}

// Regression: same as above, but the only presented token fails to verify
// outright -- connecting still must not fail, just with no pooled grants.
TEST(AuthTest, AuthenticateSetupNotRequiredConnectsDespiteFailedToken) {
  auto config = makeConfig();
  config.requireSetupToken = false;
  AuthTokenVerifier verifier(config);

  auto badToken = makeToken(makeGrants({Action::ClientSetup}, {}, {}));
  badToken.tokenValue.back() ^= 0x01; // corrupt signature

  Parameters params(FrameType::CLIENT_SETUP);
  ASSERT_TRUE(
      params
          .insertParam(
              Parameter(static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN), badToken)
          )
          .hasValue()
  );

  auto result = authenticateSetup(verifier, params);
  ASSERT_TRUE(result.hasValue());
  ASSERT_TRUE(result.value());
  EXPECT_TRUE(result.value()->empty());
}

// --- max_tokens_per_message: caps per-message verification cost ---

// Regression: a message over the cap is rejected outright, not truncated to
// the first N tokens -- a covering token past the cap must not silently lose
// to a non-covering one that happened to arrive first.
TEST(AuthTest, AuthorizeRejectsRequestTokensBeyondMaxTokensPerMessage) {
  TrackNamespace ns{{"live"}};
  auto config = makeConfig();
  config.maxTokensPerMessage = 1;
  AuthTokenVerifier verifier(config);

  Parameters params(FrameType::SUBSCRIBE);
  ASSERT_TRUE(params
                  .insertParam(Parameter(
                      static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN),
                      makeToken(makeGrants({Action::Publish}, {}, {})) // 1st token: wrong action
                  ))
                  .hasValue());
  ASSERT_TRUE(
      params
          .insertParam(Parameter(
              static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN),
              makeToken(makeGrants({Action::Subscribe}, {}, {})) // 2nd token: would grant it
          ))
          .hasValue()
  );
  std::vector<Grants> sessionGrants; // doesn't cover the action either

  // 2 tokens > cap of 1: the whole message is rejected, regardless of what
  // either token would have granted.
  auto result = authorize(verifier, Action::Subscribe, params, ns, sessionGrants, "video");
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error(), AuthError::TooManyTokens);
}

// Regression: disabled allow_request_token_override must not exempt a
// message from the cap -- the cap is checked before request tokens are
// considered at all, so an anonymous_claim that would otherwise permit the
// action can't rescue an over-cap message.
TEST(AuthTest, AuthorizeEnforcesTokenCapWhenOverrideDisabled) {
  TrackNamespace ns{{"live"}};
  auto config = makeConfig();
  config.allowRequestTokenOverride = false;
  config.maxTokensPerMessage = 1;
  config.anonymousClaim = {makeAnonymousConfigScope({Action::Subscribe})};
  AuthTokenVerifier verifier(config);

  Parameters params(FrameType::SUBSCRIBE);
  ASSERT_TRUE(params
                  .insertParam(Parameter(
                      static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN),
                      makeToken(makeGrants({Action::Publish}, {}, {}))
                  ))
                  .hasValue());
  ASSERT_TRUE(params
                  .insertParam(Parameter(
                      static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN),
                      makeToken(makeGrants({Action::Subscribe}, {}, {}))
                  ))
                  .hasValue());
  std::vector<Grants> sessionGrants;

  auto result = authorize(verifier, Action::Subscribe, params, ns, sessionGrants, "video");
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error(), AuthError::TooManyTokens);
}

TEST(AuthTest, AuthenticateSetupRejectsSetupTokensBeyondMaxTokensPerMessage) {
  auto config = makeConfig();
  config.maxTokensPerMessage = 1;
  AuthTokenVerifier verifier(config);

  Parameters params(FrameType::CLIENT_SETUP);
  ASSERT_TRUE(
      params
          .insertParam(Parameter(
              static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN),
              makeToken(makeGrants({Action::Subscribe}, {}, {})) // 1st token: not ClientSetup
          ))
          .hasValue()
  );
  ASSERT_TRUE(
      params
          .insertParam(Parameter(
              static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN),
              makeToken(makeGrants({Action::ClientSetup}, {}, {})) // 2nd token: would grant it
          ))
          .hasValue()
  );

  // 2 tokens > cap of 1: the whole message is rejected, regardless of what
  // either token would have granted.
  auto result = authenticateSetup(verifier, params);
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error(), AuthError::TooManyTokens);
}

// --- Anonymous claim: a static, config-driven floor ---

TEST(AuthTest, AuthorizeAnonymousClaimCoversWhatNeitherSessionNorRequestTokenCover) {
  TrackNamespace ns{{"live"}};
  auto config = makeConfig();
  config.anonymousClaim = {makeAnonymousConfigScope({Action::Fetch})};
  AuthTokenVerifier verifier(config);

  Parameters params(FrameType::FETCH);                                        // no request token
  std::vector<Grants> sessionGrants{makeGrants({Action::Subscribe}, {}, {})}; // doesn't cover Fetch

  auto result = authorize(verifier, Action::Fetch, params, ns, sessionGrants, "video");
  EXPECT_TRUE(result.hasValue());
}

TEST(AuthTest, AuthorizeAnonymousClaimDoesNotCoverActionsOutsideItsScope) {
  TrackNamespace ns{{"live"}};
  auto config = makeConfig();
  config.anonymousClaim = {makeAnonymousConfigScope({Action::Fetch})};
  AuthTokenVerifier verifier(config);

  Parameters params(FrameType::PUBLISH); // no request token
  std::vector<Grants> sessionGrants;     // empty

  auto result = authorize(verifier, Action::Publish, params, ns, sessionGrants, "video");
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error(), AuthError::Forbidden);
}

// The anonymous claim never satisfies the ClientSetup gate, even if
// (hypothetically, bypassing config validation) it were configured to grant
// it -- authenticateSetup() never consults allowsAnonymous() at all.
TEST(AuthTest, AuthenticateSetupAnonymousClaimNeverSatisfiesClientSetupGate) {
  auto config = makeConfig();
  config.anonymousClaim = {makeAnonymousConfigScope({Action::ClientSetup})};
  AuthTokenVerifier verifier(config);

  Parameters params(FrameType::CLIENT_SETUP); // no setup token at all
  auto result = authenticateSetup(verifier, params);
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error(), AuthError::Missing);
}

TEST(AuthTest, AuthorizeAnonymousClaimNamespaceMatchRestrictsToConfiguredPrefix) {
  auto config = makeConfig();
  config.anonymousClaim = {config::AuthConfig::AnonymousScope{
      .actions = {Action::Fetch},
      .namespaceSegments = std::vector<std::string>{"live"},
      .namespaceMatchMode = MatchRuleType::Prefix,
  }};
  AuthTokenVerifier verifier(config);

  Parameters params(FrameType::FETCH); // no request token
  std::vector<Grants> sessionGrants;   // empty

  auto liveResult = authorize(
      verifier,
      Action::Fetch,
      params,
      TrackNamespace{{"live", "event1"}},
      sessionGrants,
      "video"
  );
  EXPECT_TRUE(liveResult.hasValue());

  auto otherResult =
      authorize(verifier, Action::Fetch, params, TrackNamespace{{"vod"}}, sessionGrants, "video");
  ASSERT_TRUE(otherResult.hasError());
  EXPECT_EQ(otherResult.error(), AuthError::Forbidden);
}

// Regression: an explicitly-configured empty namespace segment list (e.g.
// `namespace_match: {exact: []}`) must restrict to the zero-segment
// namespace, not silently behave like "no namespace_match configured".
TEST(AuthTest, AuthorizeAnonymousClaimEmptyNamespaceListIsRestrictive) {
  auto config = makeConfig();
  config.anonymousClaim = {makeAnonymousConfigScope({Action::Fetch}, std::vector<std::string>{})};
  AuthTokenVerifier verifier(config);

  Parameters params(FrameType::FETCH); // no request token
  std::vector<Grants> sessionGrants;   // empty

  auto result =
      authorize(verifier, Action::Fetch, params, TrackNamespace{{"live"}}, sessionGrants, "video");
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error(), AuthError::Forbidden);
}

namespace {
Grants replayGrants(std::string cti) {
  auto grants = makeGrants({Action::ClientSetup}, {}, {});
  grants.cti = std::move(cti);
  grants.replayProtected = true;
  return grants;
}
} // namespace

TEST(AuthTest, ReplayProtectedTokenBindsToFirstConnection) {
  AuthTokenVerifier verifier(makeConfig());
  const auto token = makeToken(replayGrants("id-1"));

  EXPECT_TRUE(verifier.verify(token, 1).hasValue());
  EXPECT_TRUE(verifier.verify(token, 1).hasValue()); // reuse on the same connection
  auto other = verifier.verify(token, 2);
  ASSERT_TRUE(other.hasError());
  EXPECT_EQ(other.error(), AuthError::Replayed);
  // A different token is independent.
  EXPECT_TRUE(verifier.verify(makeToken(replayGrants("id-2")), 2).hasValue());
}

TEST(AuthTest, ReplayProtectedTokenNeedsConnectionIdentity) {
  AuthTokenVerifier verifier(makeConfig());
  EXPECT_TRUE(verifier.verify(makeToken(replayGrants("id-3")), 0).hasError());
}

TEST(AuthTest, UnverifiedReplayTokenDoesNotReserveCti) {
  AuthTokenVerifier verifier(makeConfig());
  // Signed with the wrong key: must not bind the cti to connection 9.
  EXPECT_TRUE(verifier.verify(makeToken(replayGrants("id-4"), "wrong"), 9).hasError());
  EXPECT_TRUE(verifier.verify(makeToken(replayGrants("id-4")), 1).hasValue());
}

TEST(AuthTest, ReplayBindingIsPerConnectionInAuthenticateSetupAndAuthorize) {
  AuthTokenVerifier verifier(makeConfig());
  Parameters params(FrameType::CLIENT_SETUP);
  ASSERT_TRUE(params
                  .insertParam(Parameter(
                      static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN),
                      makeToken(replayGrants("id-5"))
                  ))
                  .hasValue());
  EXPECT_TRUE(authenticateSetup(verifier, params, 1).hasValue());
  EXPECT_TRUE(authenticateSetup(verifier, params, 1).hasValue());
  auto stolen = authenticateSetup(verifier, params, 2);
  ASSERT_TRUE(stolen.hasError());
  EXPECT_EQ(stolen.error(), AuthError::Replayed);
}

// ---- DPoP integration ----

namespace {
std::string dpopKeyPath() {
  return std::string(TEST_C4M_KEY_DIR) + "/c4m_es256_private.pem";
}

Grants dpopGrants(std::vector<Action> actions, const std::string& keyPath = dpopKeyPath()) {
  auto grants = makeGrants(std::move(actions), {}, {});
  grants.dpop = DpopRequirement{
      DpopBinding{DpopBinding::Kind::Ckt, coseKeyThumbprint(dpopPublicKey(keyPath))},
      std::chrono::seconds(60),
      true,
  };
  return grants;
}

void addParam(Parameters& params, uint64_t type, std::string value) {
  ASSERT_TRUE(params
                  .insertParam(Parameter(
                      static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN),
                      AuthToken{
                          .tokenType = type,
                          .tokenValue = std::move(value),
                          .alias = AuthToken::DontRegister
                      }
                  ))
                  .hasValue());
}

std::string proofFor(
    const AuthToken& token,
    Action action,
    TrackNamespace ns = {},
    std::optional<std::string> track = std::nullopt,
    std::string cti = "p1"
) {
  return signDpopProof(
      dpopKeyPath(),
      token.tokenValue,
      DpopContext{action, ns.trackNamespace, std::move(track)},
      cti
  );
}
} // namespace

TEST(AuthTest, DpopBoundTokenNeedsProofAtSetupAndPerAction) {
  AuthTokenVerifier verifier(makeConfig());
  const auto token = makeToken(dpopGrants({Action::ClientSetup, Action::Subscribe, Action::Fetch}));
  const TrackNamespace ns{{"live"}};

  // SETUP without a proof is refused; the cnf-bound token is not a bearer token.
  Parameters bare(FrameType::CLIENT_SETUP);
  addParam(bare, 77, token.tokenValue);
  EXPECT_TRUE(authenticateSetup(verifier, bare, 1).hasError());

  Parameters setup(FrameType::CLIENT_SETUP);
  addParam(setup, 77, token.tokenValue);
  addParam(setup, 17, proofFor(token, Action::ClientSetup));
  auto grants = authenticateSetup(verifier, setup, 1);
  ASSERT_TRUE(grants.hasValue());

  // A later request on the session has no token of its own, so it needs its own proof.
  Parameters noProof(FrameType::SUBSCRIBE);
  EXPECT_TRUE(
      authorize(verifier, Action::Subscribe, noProof, ns, *grants.value(), "video", 1).hasError()
  );

  Parameters ok(FrameType::SUBSCRIBE);
  addParam(ok, 17, proofFor(token, Action::Subscribe, ns, "video", "p2"));
  EXPECT_TRUE(authorize(verifier, Action::Subscribe, ok, ns, *grants.value(), "video", 1).hasValue()
  );

  // A proof is for one action and one track.
  Parameters wrongAction(FrameType::SUBSCRIBE);
  addParam(wrongAction, 17, proofFor(token, Action::Fetch, ns, "video", "p3"));
  EXPECT_TRUE(authorize(verifier, Action::Subscribe, wrongAction, ns, *grants.value(), "video", 1)
                  .hasError());
  Parameters wrongTrack(FrameType::SUBSCRIBE);
  addParam(wrongTrack, 17, proofFor(token, Action::Subscribe, ns, "audio", "p4"));
  EXPECT_TRUE(
      authorize(verifier, Action::Subscribe, wrongTrack, ns, *grants.value(), "video", 1).hasError()
  );
}

TEST(AuthTest, DpopProofCtiCannotBeReplayed) {
  AuthTokenVerifier verifier(makeConfig());
  const auto token = makeToken(dpopGrants({Action::ClientSetup}));
  Parameters setup(FrameType::CLIENT_SETUP);
  addParam(setup, 77, token.tokenValue);
  addParam(setup, 17, proofFor(token, Action::ClientSetup, {}, std::nullopt, "same"));
  EXPECT_TRUE(authenticateSetup(verifier, setup, 1).hasValue());
  EXPECT_TRUE(authenticateSetup(verifier, setup, 2).hasError());
}

TEST(AuthTest, DpopProofForAnotherTokenOrKeyIsRejected) {
  AuthTokenVerifier verifier(makeConfig());
  const auto token = makeToken(dpopGrants({Action::ClientSetup}));
  const auto other = makeToken(dpopGrants({Action::ClientSetup, Action::Fetch}));
  Parameters setup(FrameType::CLIENT_SETUP);
  addParam(setup, 77, token.tokenValue);
  addParam(setup, 17, proofFor(other, Action::ClientSetup));
  EXPECT_TRUE(authenticateSetup(verifier, setup, 1).hasError());
}

TEST(AuthTest, TokenWithoutCnfIgnoresProofs) {
  AuthTokenVerifier verifier(makeConfig());
  const auto token = makeToken(makeGrants({Action::ClientSetup}, {}, {}));
  Parameters setup(FrameType::CLIENT_SETUP);
  addParam(setup, 77, token.tokenValue);
  EXPECT_TRUE(authenticateSetup(verifier, setup, 1).hasValue());
}

// ---- moqt-reval: which grants keep an action alive ----

namespace {
Grants revalGrants(std::chrono::seconds expiresIn, std::chrono::seconds every) {
  auto grants = makeGrants({Action::Subscribe}, {}, {});
  grants.expiresAt = std::chrono::system_clock::now() + expiresIn;
  grants.revalidateEvery = every;
  return grants;
}

Parameters noParams() {
  return Parameters(FrameType::SUBSCRIBE);
}
} // namespace

TEST(AuthTest, AuthorizeReportsRevalidationForAnExpiringRevalidatedGrant) {
  AuthTokenVerifier verifier(makeConfig());
  const auto grants =
      std::vector<Grants>{revalGrants(std::chrono::seconds(100), std::chrono::seconds(10))};
  auto decision =
      authorize(verifier, Action::Subscribe, noParams(), TrackNamespace{{"live"}}, grants, "v");
  ASSERT_TRUE(decision.hasValue());
  ASSERT_TRUE(decision.value().revalidation.has_value());
  EXPECT_EQ(decision.value().revalidation->interval, std::chrono::seconds(10));
  EXPECT_EQ(decision.value().revalidation->deadline, grants[0].expiresAt);
}

TEST(AuthTest, AuthorizeReportsNoRevalidationWhenAnyPermittingSourceDoesNotExpire) {
  AuthTokenVerifier verifier(makeConfig());
  auto plain = makeGrants({Action::Subscribe}, {}, {});
  auto decision = authorize(
      verifier,
      Action::Subscribe,
      noParams(),
      TrackNamespace{{"live"}},
      std::vector<Grants>{revalGrants(std::chrono::seconds(100), std::chrono::seconds(10)), plain},
      "v"
  );
  ASSERT_TRUE(decision.hasValue());
  EXPECT_FALSE(decision.value().revalidation.has_value());

  // Revalidated grants that do not permit the action are irrelevant to it.
  auto other = revalGrants(std::chrono::seconds(100), std::chrono::seconds(10));
  other.scopes[0].actions = {Action::Fetch};
  auto onlyOther = authorize(
      verifier,
      Action::Subscribe,
      noParams(),
      TrackNamespace{{"live"}},
      std::vector<Grants>{other, plain},
      "v"
  );
  ASSERT_TRUE(onlyOther.hasValue());
  EXPECT_FALSE(onlyOther.value().revalidation.has_value());
}

TEST(AuthTest, AuthorizeCombinesRevalidatedGrantsByLatestExpiryAndShortestInterval) {
  AuthTokenVerifier verifier(makeConfig());
  const auto a = revalGrants(std::chrono::seconds(100), std::chrono::seconds(30));
  const auto b = revalGrants(std::chrono::seconds(500), std::chrono::seconds(10));
  auto decision = authorize(
      verifier,
      Action::Subscribe,
      noParams(),
      TrackNamespace{{"live"}},
      std::vector<Grants>{a, b},
      "v"
  );
  ASSERT_TRUE(decision.hasValue());
  ASSERT_TRUE(decision.value().revalidation.has_value());
  EXPECT_EQ(decision.value().revalidation->deadline, b.expiresAt);
  EXPECT_EQ(decision.value().revalidation->interval, std::chrono::seconds(10));
}

TEST(AuthTest, AuthorizeWithoutRevalidationClaimIsUnchanged) {
  AuthTokenVerifier verifier(makeConfig());
  auto decision = authorize(
      verifier,
      Action::Subscribe,
      noParams(),
      TrackNamespace{{"live"}},
      std::vector<Grants>{makeGrants({Action::Subscribe}, {}, {})},
      "v"
  );
  ASSERT_TRUE(decision.hasValue());
  EXPECT_FALSE(decision.value().revalidation.has_value());
}
