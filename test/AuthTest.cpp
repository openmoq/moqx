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

void appendTypeLen(std::string& out, uint8_t major, uint64_t len) {
  if (len < 24) {
    out.push_back(static_cast<char>((major << 5) | len));
  } else if (len <= 0xff) {
    out.push_back(static_cast<char>((major << 5) | 24));
    out.push_back(static_cast<char>(len));
  } else if (len <= 0xffff) {
    out.push_back(static_cast<char>((major << 5) | 25));
    out.push_back(static_cast<char>((len >> 8) & 0xff));
    out.push_back(static_cast<char>(len & 0xff));
  } else {
    out.push_back(static_cast<char>((major << 5) | 26));
    out.push_back(static_cast<char>((len >> 24) & 0xff));
    out.push_back(static_cast<char>((len >> 16) & 0xff));
    out.push_back(static_cast<char>((len >> 8) & 0xff));
    out.push_back(static_cast<char>(len & 0xff));
  }
}

std::string cborUInt(uint64_t value) {
  std::string out;
  appendTypeLen(out, 0, value);
  return out;
}

std::string cborText(std::string_view value) {
  std::string out;
  appendTypeLen(out, 3, value.size());
  out.append(value);
  return out;
}

std::string cborBytes(std::string_view value) {
  std::string out;
  appendTypeLen(out, 2, value.size());
  out.append(value);
  return out;
}

std::string cborArray(std::initializer_list<std::string_view> values) {
  std::string out;
  appendTypeLen(out, 4, values.size());
  for (auto value : values) {
    out.append(value);
  }
  return out;
}

std::string cborMap(std::initializer_list<std::pair<std::string_view, std::string_view>> entries) {
  std::string out;
  appendTypeLen(out, 5, entries.size());
  for (const auto& [key, value] : entries) {
    out.append(key);
    out.append(value);
  }
  return out;
}

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

AuthToken
makeToken(std::string claims, std::string_view secret = "secret", std::string_view keyID = "k1") {
  return AuthToken{
      .tokenType = 77,
      .tokenValue = AuthTokenVerifier::signForTest(keyID, secret, claims),
      .alias = AuthToken::DontRegister,
  };
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

std::string claimsFor(Action action, const TrackNamespace& ns, std::string_view track) {
  auto actions = cborArray({cborUInt(static_cast<uint64_t>(action))});
  auto nsMatch = cborMap({{cborUInt(0), cborBytes(namespaceBytes(ns))}});
  auto trackMatch = cborMap({{cborUInt(0), cborBytes(track)}});
  auto scope = cborArray({actions, nsMatch, trackMatch});
  auto moqt = cborArray({scope});
  auto exp = cborUInt(4'102'444'800ULL); // 2100-01-01
  return cborMap({{cborUInt(4), exp}, {cborText("moqt"), moqt}});
}

std::string
claimsWithScope(std::string_view actions, std::string_view nsMatch, std::string_view trackMatch) {
  auto scope = cborArray({actions, nsMatch, trackMatch});
  auto exp = cborUInt(4'102'444'800ULL); // 2100-01-01
  return cborMap({{cborUInt(4), exp}, {cborText("moqt"), cborArray({scope})}});
}

} // namespace

TEST(AuthTest, VerifiesSignedTokenAndAllowsMatchingAction) {
  TrackNamespace ns{{"live", "event"}};
  AuthTokenVerifier verifier(makeConfig());
  auto grants = verifier.verify(makeToken(claimsFor(Action::Subscribe, ns, "video")));
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
  auto token = makeToken(claimsFor(Action::ClientSetup, TrackNamespace{}, ""));
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
  auto grants = verifier.verify(
      makeToken(claimsFor(Action::ClientSetup, TrackNamespace{}, ""), "secret-2", "k2")
  );
  ASSERT_TRUE(grants.hasValue());

  auto missingKey = verifier.verify(
      makeToken(claimsFor(Action::ClientSetup, TrackNamespace{}, ""), "secret-3", "k3")
  );
  ASSERT_TRUE(missingKey.hasError());
  EXPECT_EQ(missingKey.error(), AuthError::BadSignature);
}

TEST(AuthTest, RejectsWrongTokenType) {
  auto token = makeToken(claimsFor(Action::ClientSetup, TrackNamespace{}, ""));
  token.tokenType = 78;
  AuthTokenVerifier verifier(makeConfig());

  auto grants = verifier.verify(token);
  ASSERT_TRUE(grants.hasError());
  EXPECT_EQ(grants.error(), AuthError::WrongTokenType);
}

TEST(AuthTest, RejectsMalformedTokenEnvelope) {
  AuthTokenVerifier verifier(makeConfig());

  auto empty = makeToken(claimsFor(Action::ClientSetup, TrackNamespace{}, ""));
  empty.tokenValue.clear();
  auto emptyRes = verifier.verify(empty);
  ASSERT_TRUE(emptyRes.hasError());
  EXPECT_EQ(emptyRes.error(), AuthError::Malformed);

  auto wrongVersion = makeToken(claimsFor(Action::ClientSetup, TrackNamespace{}, ""));
  wrongVersion.tokenValue[0] = '\x02';
  auto wrongVersionRes = verifier.verify(wrongVersion);
  ASSERT_TRUE(wrongVersionRes.hasError());
  EXPECT_EQ(wrongVersionRes.error(), AuthError::Malformed);

  auto truncated = makeToken(claimsFor(Action::ClientSetup, TrackNamespace{}, ""));
  truncated.tokenValue.resize(3);
  auto truncatedRes = verifier.verify(truncated);
  ASSERT_TRUE(truncatedRes.hasError());
  EXPECT_EQ(truncatedRes.error(), AuthError::Malformed);

  auto claimsLenOverflow = makeToken(claimsFor(Action::ClientSetup, TrackNamespace{}, ""));
  const auto claimsLenOffset = size_t{2} + std::string_view("k1").size();
  claimsLenOverflow.tokenValue[claimsLenOffset] = '\x7f';
  auto claimsLenOverflowRes = verifier.verify(claimsLenOverflow);
  ASSERT_TRUE(claimsLenOverflowRes.hasError());
  EXPECT_EQ(claimsLenOverflowRes.error(), AuthError::Malformed);
}

TEST(AuthTest, RejectsMalformedClaims) {
  AuthTokenVerifier verifier(makeConfig());
  auto token = makeToken(cborArray({}));

  auto grants = verifier.verify(token);
  ASSERT_TRUE(grants.hasError());
  EXPECT_EQ(grants.error(), AuthError::Malformed);
}

TEST(AuthTest, RejectsDuplicateWellKnownClaimKeys) {
  AuthTokenVerifier verifier(makeConfig());

  // exp present twice must be rejected rather than silently last-wins.
  auto dupExp = cborMap({
      {cborUInt(4), cborUInt(4'102'444'800ULL)},
      {cborUInt(4), cborUInt(4'102'444'800ULL)},
  });
  auto dupExpRes = verifier.verify(makeToken(dupExp));
  ASSERT_TRUE(dupExpRes.hasError());
  EXPECT_EQ(dupExpRes.error(), AuthError::Malformed);

  // moqt present twice must be rejected too.
  auto scope = cborArray(
      {cborArray({cborUInt(static_cast<uint64_t>(Action::ClientSetup))}), cborMap({}), cborMap({})}
  );
  auto dupMoqt = cborMap({
      {cborUInt(4), cborUInt(4'102'444'800ULL)},
      {cborText("moqt"), cborArray({scope})},
      {cborText("moqt"), cborArray({scope})},
  });
  auto dupMoqtRes = verifier.verify(makeToken(dupMoqt));
  ASSERT_TRUE(dupMoqtRes.hasError());
  EXPECT_EQ(dupMoqtRes.error(), AuthError::Malformed);
}

TEST(AuthTest, RejectsExpiredToken) {
  auto scope = cborArray(
      {cborArray({cborUInt(static_cast<uint64_t>(Action::ClientSetup))}), cborMap({}), cborMap({})}
  );
  auto claims = cborMap({{cborUInt(4), cborUInt(1)}, {cborText("moqt"), cborArray({scope})}});
  AuthTokenVerifier verifier(makeConfig());
  auto grants = verifier.verify(makeToken(claims));
  ASSERT_TRUE(grants.hasError());
  EXPECT_EQ(grants.error(), AuthError::Expired);
}

TEST(AuthTest, ParsesScalarActionAndMoqtRevalClaim) {
  auto claims = claimsWithScope(
      cborUInt(static_cast<uint64_t>(Action::ClientSetup)),
      cborMap({}),
      cborMap({})
  );
  auto withReval = cborMap({
      {cborUInt(4), cborUInt(4'102'444'800ULL)},
      {cborText("moqt"),
       cborArray({cborArray(
           {cborUInt(static_cast<uint64_t>(Action::ClientSetup)), cborMap({}), cborMap({})}
       )})},
      {cborText("moqt-reval"), cborUInt(60)},
  });

  AuthTokenVerifier verifier(makeConfig());
  auto scalarGrants = verifier.verify(makeToken(claims));
  ASSERT_TRUE(scalarGrants.hasValue());
  EXPECT_TRUE(allows(scalarGrants.value(), Action::ClientSetup, TrackNamespace{}));

  auto revalGrants = verifier.verify(makeToken(withReval));
  ASSERT_TRUE(revalGrants.hasValue());
  EXPECT_TRUE(allows(revalGrants.value(), Action::ClientSetup, TrackNamespace{}));
}
