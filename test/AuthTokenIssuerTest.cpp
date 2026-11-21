/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "auth/AuthTokenIssuer.h"

#include "auth/Auth.h"

#include <folly/portability/GTest.h>

using namespace openmoq::moqx;
using namespace openmoq::moqx::auth;
using namespace moxygen;

TEST(AuthTokenIssuerTest, C4mDraft02UsesSetupActionOne) {
  EXPECT_EQ(static_cast<uint64_t>(Action::ClientSetup), 1);
  EXPECT_THROW(parseActions("server_setup"), std::invalid_argument);
  EXPECT_THROW(parseActions("0"), std::invalid_argument);
}

TEST(AuthTokenIssuerTest, C4mDraft02IssuesTaggedCoseMac0) {
  auto issued = issueToken(IssueTokenOptions{
      .keyID = "c4m-dev",
      .secret = "operator-test-secret-with-enough-entropy",
      .actions = {Action::ClientSetup},
  });
  ASSERT_FALSE(issued.tokenValue.empty());
  EXPECT_EQ(static_cast<uint8_t>(issued.tokenValue.front()), 0xd1);
}

TEST(AuthTokenIssuerTest, Es256IssuerWorksWithPublicOnlyRelayConfiguration) {
  auto issued = issueToken(IssueTokenOptions{
      .keyID = "es-key",
      .actions = {Action::ClientSetup, Action::Publish},
      .trackNamespace = TrackNamespace{{"live", "event"}},
      .trackName = "video",
      .privateKeyFile = std::string(TEST_C4M_KEY_DIR) + "/c4m_es256_private.pem",
  });
  auto config = config::AuthConfig{
      .enabled = true,
      .es256Keys =
          {{.id = "es-key", .publicKeyFile = std::string(TEST_C4M_KEY_DIR) + "/c4m_es256_public.pem"
          }},
  };
  AuthTokenVerifier verifier(config);
  auto token =
      AuthToken{.tokenType = 1, .tokenValue = issued.tokenValue, .alias = AuthToken::DontRegister};
  auto result = verifier.verify(token);
  ASSERT_TRUE(result.hasValue());
  EXPECT_TRUE(config.hmacKeys.empty());
  EXPECT_TRUE(allows(result.value(), Action::ClientSetup, TrackNamespace{}));
  EXPECT_TRUE(allows(
      result.value(),
      Action::Publish,
      FullTrackName{TrackNamespace{{"live", "event"}}, "video"}
  ));
  EXPECT_FALSE(allows(
      result.value(),
      Action::Publish,
      FullTrackName{TrackNamespace{{"live", "event"}}, "audio"}
  ));
  token.tokenValue.back() ^= 1;
  auto corrupt = verifier.verify(token);
  ASSERT_TRUE(corrupt.hasError());
  EXPECT_EQ(corrupt.error(), AuthError::BadSignature);
}

TEST(AuthTokenIssuerTest, RejectsAmbiguousHmacAndEs256SigningOptions) {
  EXPECT_THROW(
      issueToken(IssueTokenOptions{
          .keyID = "es-key",
          .secret = "secret",
          .actions = {Action::ClientSetup},
          .privateKeyFile = std::string(TEST_C4M_KEY_DIR) + "/c4m_es256_private.pem",
      }),
      std::invalid_argument
  );
}

TEST(AuthTokenIssuerTest, IssuesSetupAndPublishTokenAcceptedByVerifier) {
  const std::string secret = "operator-test-secret-with-enough-entropy";
  IssueTokenOptions options{
      .keyID = "cat-dev",
      .secret = secret,
      .actions = {Action::ClientSetup, Action::PublishNamespace, Action::Publish},
      .trackNamespace = TrackNamespace{{"c4m.example"}},
      .trackName = "video",
      .ttl = std::chrono::seconds(3600),
      .now = std::chrono::system_clock::time_point(std::chrono::seconds(1'800'000'000)),
  };

  auto issued = issueToken(options);

  AuthTokenVerifier verifier(config::AuthConfig{
      .enabled = true,
      .tokenType = 1,
      .hmacKeys = {config::AuthConfig::HmacKey{.id = "cat-dev", .secret = secret}},
      .requireSetupToken = true,
      .allowRequestTokenOverride = true,
  });
  auto grants = verifier.verify(AuthToken{
      .tokenType = 1,
      .tokenValue = issued.tokenValue,
      .alias = AuthToken::DontRegister,
  });

  ASSERT_TRUE(grants.hasValue());
  EXPECT_TRUE(allows(grants.value(), Action::ClientSetup, TrackNamespace{}));
  EXPECT_TRUE(allows(grants.value(), Action::PublishNamespace, TrackNamespace{{"c4m.example"}}));
  EXPECT_TRUE(allows(
      grants.value(),
      Action::Publish,
      FullTrackName{TrackNamespace{{"c4m.example"}}, "video"}
  ));
  EXPECT_FALSE(allows(
      grants.value(),
      Action::Publish,
      FullTrackName{TrackNamespace{{"c4m.example"}}, "audio"}
  ));
}

TEST(AuthTokenIssuerTest, ParsesOperatorActionNames) {
  auto actions = parseActions("client_setup,publish_namespace,publish");

  ASSERT_EQ(actions.size(), 3);
  EXPECT_EQ(actions[0], Action::ClientSetup);
  EXPECT_EQ(actions[1], Action::PublishNamespace);
  EXPECT_EQ(actions[2], Action::Publish);
}

TEST(AuthTokenIssuerTest, ParsesSlashSeparatedNamespace) {
  auto ns = parseTrackNamespace("live/event/main");

  ASSERT_EQ(ns.trackNamespace.size(), 3);
  EXPECT_EQ(ns.trackNamespace[0], "live");
  EXPECT_EQ(ns.trackNamespace[1], "event");
  EXPECT_EQ(ns.trackNamespace[2], "main");
}

TEST(AuthTokenIssuerTest, SelectsConfiguredHmacKeyByID) {
  config::AuthConfig auth{
      .enabled = true,
      .tokenType = 1,
      .hmacKeys =
          {
              config::AuthConfig::HmacKey{.id = "old", .secret = "old-secret"},
              config::AuthConfig::HmacKey{.id = "active", .secret = "active-secret"},
          },
  };

  auto key = selectHmacKey(auth, "active");

  EXPECT_EQ(key.id, "active");
  EXPECT_EQ(key.secret, "active-secret");
}

TEST(AuthTokenIssuerTest, IssuedRevalidationIntervalIsAcceptedByARelayThatAllowsIt) {
  auto issued = issueToken(IssueTokenOptions{
      .keyID = "k",
      .secret = "operator-test-secret-with-enough-entropy",
      .actions = {Action::ClientSetup},
      .revalidateEvery = std::chrono::seconds(30),
  });
  const auto token = moxygen::AuthToken{
      .tokenType = 1,
      .tokenValue = issued.tokenValue,
      .alias = moxygen::AuthToken::DontRegister,
  };
  auto allows = config::AuthConfig{
      .enabled = true,
      .hmacKeys = {{.id = "k", .secret = "operator-test-secret-with-enough-entropy"}},
      .minRevalidationSeconds = 5,
      .maxTokensPerMessage = 4,
  };
  auto accepted = AuthTokenVerifier(allows).verify(token);
  ASSERT_TRUE(accepted.hasValue());
  EXPECT_EQ(accepted.value().revalidateEvery, std::chrono::seconds(30));
  allows.minRevalidationSeconds = 0; // revalidation disabled: such tokens are refused
  EXPECT_TRUE(AuthTokenVerifier(allows).verify(token).hasError());
}
