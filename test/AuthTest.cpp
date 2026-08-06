/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "auth/Auth.h"
#include "auth/AuthTokenIssuer.h"

#include <folly/portability/GTest.h>

using namespace openmoq::moqx;
using namespace openmoq::moqx::auth;
using namespace moxygen;

namespace {

std::string namespaceBytes(const TrackNamespace& ns) {
  std::string out;
  for (const auto& field : ns.trackNamespace) {
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);
    out.push_back(static_cast<char>(field.size()));
    out.append(field);
  }
  return out;
}

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

TEST(AuthTest, VerifiesSignedTokenAndAllowsMatchingAction) {
  TrackNamespace ns{{"live", "event"}};
  AuthTokenVerifier verifier(makeConfig());
  auto token = makeToken(makeGrants(
      {Action::Subscribe},
      {MatchRule{.type = MatchRule::Type::Exact, .value = namespaceBytes(ns)}},
      {MatchRule{.type = MatchRule::Type::Exact, .value = "video"}}
  ));
  auto grants = verifier.verify(token);
  ASSERT_TRUE(grants.hasValue());
  EXPECT_TRUE(allows(grants.value(), Action::Subscribe, FullTrackName{ns, "video"}));
  EXPECT_FALSE(allows(grants.value(), Action::Publish, FullTrackName{ns, "video"}));
  EXPECT_FALSE(allows(grants.value(), Action::Subscribe, FullTrackName{ns, "audio"}));
}

TEST(AuthTest, AllowsPrefixSuffixAndContainsMatchRules) {
  TrackNamespace ns{{"live", "event"}};
  const auto canonicalNs = namespaceBytes(ns);

  auto prefixGrants = makeGrants(
      {Action::Subscribe},
      {MatchRule{.type = MatchRule::Type::Prefix, .value = canonicalNs.substr(0, 8)}},
      {MatchRule{.type = MatchRule::Type::Prefix, .value = "vid"}}
  );
  EXPECT_TRUE(allows(prefixGrants, Action::Subscribe, FullTrackName{ns, "video"}));
  EXPECT_FALSE(
      allows(prefixGrants, Action::Subscribe, FullTrackName{TrackNamespace{{"vod"}}, "video"})
  );

  auto suffixGrants = makeGrants(
      {Action::Fetch},
      {MatchRule{
          .type = MatchRule::Type::Suffix,
          .value = canonicalNs.substr(canonicalNs.size() - 9)
      }},
      {MatchRule{.type = MatchRule::Type::Suffix, .value = ".mp4"}}
  );
  EXPECT_TRUE(allows(suffixGrants, Action::Fetch, FullTrackName{ns, "clip.mp4"}));
  EXPECT_FALSE(allows(suffixGrants, Action::Fetch, FullTrackName{ns, "clip.m4s"}));

  auto containsGrants = makeGrants(
      {Action::Publish},
      {MatchRule{.type = MatchRule::Type::Contains, .value = "live"}},
      {MatchRule{.type = MatchRule::Type::Contains, .value = "main"}}
  );
  EXPECT_TRUE(allows(containsGrants, Action::Publish, FullTrackName{ns, "camera-main"}));
  EXPECT_FALSE(allows(containsGrants, Action::Publish, FullTrackName{ns, "camera-side"}));
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

TEST(AuthTest, VerifiesCatapultCwtWithOpenScope) {
  AuthTokenVerifier verifier(makeConfig());
  auto grants = verifier.verify(makeToken(makeGrants({Action::ClientSetup}, {}, {})));

  ASSERT_TRUE(grants.hasValue());
  EXPECT_TRUE(allows(grants.value(), Action::ClientSetup, TrackNamespace{}));
}

TEST(AuthTest, MultiRuleCompoundMatchRoundtripsViaCwt) {
  // Two track-match rules (prefix AND suffix) must survive CWT serialization and
  // be enforced on verification. Both conditions must hold for allows() to pass.
  auto grants = makeGrants(
      {Action::Subscribe},
      {},
      {MatchRule{.type = MatchRule::Type::Prefix, .value = "live-"},
       MatchRule{.type = MatchRule::Type::Suffix, .value = ".mp4"}}
  );
  AuthTokenVerifier verifier(makeConfig());
  auto result = verifier.verify(makeToken(grants));
  ASSERT_TRUE(result.hasValue());
  TrackNamespace ns{};
  EXPECT_TRUE(allows(result.value(), Action::Subscribe, FullTrackName{ns, "live-stream.mp4"}));
  EXPECT_FALSE(allows(result.value(), Action::Subscribe, FullTrackName{ns, "live-stream.ts"}));
  EXPECT_FALSE(allows(result.value(), Action::Subscribe, FullTrackName{ns, "vod-stream.mp4"}));
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
// it -- authenticateSetup() never consults anonymousGrants() at all.
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
