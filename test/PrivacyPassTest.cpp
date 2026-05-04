/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "auth/PrivacyPass.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace openmoq::moqx::auth {
namespace {

using ::testing::HasSubstr;

constexpr char kPrivateKeyPem[] = R"(-----BEGIN PRIVATE KEY-----
MC4CAQAwBQYDK2VwBCIEIErhyiUzUJWurF+fXkkoWw6nF3jHicKlTV7RY8klhNaH
-----END PRIVATE KEY-----)";

constexpr char kPublicKeyPem[] = R"(-----BEGIN PUBLIC KEY-----
MCowBQYDK2VwAyEAgV7mddBzR6UIIyGl/GEgWgSkImWT4bbV+LEDL2XPQnE=
-----END PUBLIC KEY-----)";

config::AuthConfig makeConfig() {
  config::AuthConfig cfg;
  cfg.enabled = true;
  cfg.audience = "moqx-test";
  cfg.issuerKeys.push_back(config::AuthIssuerKey{
      .id = "issuer-1",
      .publicKeyPem = kPublicKeyPem,
  });
  return cfg;
}

Claims makeClaims() {
  Claims claims;
  claims.issuerId = "issuer-1";
  claims.audience = "moqx-test";
  claims.issuedAt = std::chrono::system_clock::now() - std::chrono::minutes(1);
  claims.expiresAt = std::chrono::system_clock::now() + std::chrono::minutes(10);
  Scope scope;
  scope.actions = {Action::Subscribe, Action::Fetch};
  scope.namespaceMatch = MatchSpec{MatchKind::Prefix, "/live/"};
  scope.trackMatch = MatchSpec{MatchKind::Prefix, "track/"};
  claims.scopes.push_back(std::move(scope));
  return claims;
}

moxygen::AuthToken makeToken(const std::string& value) {
  return moxygen::AuthToken{
      .tokenType = 0,
      .tokenValue = value,
      .alias = moxygen::AuthToken::DontRegister,
  };
}

} // namespace

TEST(PrivacyPass, VerifiesAndAuthorizesValidToken) {
  auto config = makeConfig();
  auto verifier = PrivacyPassVerifier(config);
  auto claims = makeClaims();
  auto token = makeToken(signTokenForTest(config.issuerKeys.front(), kPrivateKeyPem, claims));

  auto verified = verifier.verify(token);
  ASSERT_TRUE(verified.hasValue());
  EXPECT_EQ(verified.value().issuerId, claims.issuerId);
  EXPECT_TRUE(verifier.allows(verified.value(), Action::Subscribe, "/live/room", "track/123"));
  EXPECT_FALSE(verifier.allows(verified.value(), Action::Publish, "/live/room", "track/123"));
}

TEST(PrivacyPass, RejectsExpiredToken) {
  auto config = makeConfig();
  PrivacyPassVerifier verifier(config);
  auto claims = makeClaims();
  claims.expiresAt = std::chrono::system_clock::now() - std::chrono::seconds(1);
  auto token = makeToken(signTokenForTest(config.issuerKeys.front(), kPrivateKeyPem, claims));

  auto verified = verifier.verify(token);
  ASSERT_TRUE(verified.hasError());
  EXPECT_EQ(verified.error(), AuthError::Expired);
}

TEST(PrivacyPass, RejectsWrongAudience) {
  auto config = makeConfig();
  PrivacyPassVerifier verifier(config);
  auto claims = makeClaims();
  claims.audience = "other";
  auto token = makeToken(signTokenForTest(config.issuerKeys.front(), kPrivateKeyPem, claims));

  auto verified = verifier.verify(token);
  ASSERT_TRUE(verified.hasError());
  EXPECT_EQ(verified.error(), AuthError::Forbidden);
}

TEST(PrivacyPass, RejectsBadSignature) {
  auto config = makeConfig();
  PrivacyPassVerifier verifier(config);
  auto claims = makeClaims();
  auto tokenValue = signTokenForTest(config.issuerKeys.front(), kPrivateKeyPem, claims);
  tokenValue.back() ^= 0x01;
  auto token = makeToken(tokenValue);

  auto verified = verifier.verify(token);
  ASSERT_TRUE(verified.hasError());
  EXPECT_EQ(verified.error(), AuthError::BadSignature);
}

TEST(PrivacyPass, AuthorizeSetupAllowsMissingToken) {
  PrivacyPassVerifier verifier(makeConfig());
  moxygen::SetupParameters params{moxygen::FrameType::CLIENT_SETUP};

  auto res = verifier.authorizeSetup(params);
  ASSERT_TRUE(res.hasValue());
}

TEST(PrivacyPass, AuthorizeRequestUsesToken) {
  auto config = makeConfig();
  PrivacyPassVerifier verifier(config);
  auto claims = makeClaims();
  auto token = signTokenForTest(config.issuerKeys.front(), kPrivateKeyPem, claims);

  moxygen::Parameters params{moxygen::FrameType::SUBSCRIBE};
  ASSERT_TRUE(params
                  .insertParam(moxygen::Parameter{
                      static_cast<uint64_t>(moxygen::TrackRequestParamKey::AUTHORIZATION_TOKEN),
                      makeToken(token)
                  })
                  .hasValue());

  auto res = verifier.authorize(params, Action::Subscribe, "/live/room", "track/123");
  ASSERT_TRUE(res.hasValue());
}

} // namespace openmoq::moqx::auth
