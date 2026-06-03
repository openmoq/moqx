/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "auth/Auth.h"

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
      .tokenValue = AuthTokenVerifier::signForTest(keyID, secret, grants),
      .alias = AuthToken::DontRegister,
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

TEST(AuthTest, FindAuthTokenSelectsMatchingAuthorizationToken) {
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

  auto token = findAuthToken(params, 77);
  ASSERT_TRUE(token.has_value());
  EXPECT_EQ(token->tokenValue, "right");
  EXPECT_FALSE(findAuthToken(params, 78).has_value());
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
