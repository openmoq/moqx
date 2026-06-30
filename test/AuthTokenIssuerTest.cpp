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

TEST(AuthTokenIssuerTest, IssuesSetupAndPublishTokenAcceptedByVerifier) {
  const std::string secret = "operator-test-secret-with-enough-entropy";
  IssueTokenOptions options{
      .keyID = "cat-dev",
      .secret = secret,
      .actions = {Action::ClientSetup, Action::PublishNamespace, Action::Publish},
      .trackNamespace = TrackNamespace{{"cat4moq.example"}},
      .trackName = "video",
      .ttl = std::chrono::seconds(3600),
      .now = std::chrono::system_clock::time_point(std::chrono::seconds(1'800'000'000)),
  };

  auto issued = issueToken(options);

  AuthTokenVerifier verifier(config::AuthConfig{
      .enabled = true,
      .tokenType = 16,
      .hmacKeys = {config::AuthConfig::HmacKey{.id = "cat-dev", .secret = secret}},
      .requireSetupToken = true,
      .allowRequestTokenOverride = true,
  });
  auto grants = verifier.verify(AuthToken{
      .tokenType = 16,
      .tokenValue = issued.tokenValue,
      .alias = AuthToken::DontRegister,
  });

  ASSERT_TRUE(grants.hasValue());
  EXPECT_TRUE(allows(grants.value(), Action::ClientSetup, TrackNamespace{}));
  EXPECT_TRUE(allows(grants.value(), Action::PublishNamespace, TrackNamespace{{"cat4moq.example"}})
  );
  EXPECT_TRUE(allows(
      grants.value(),
      Action::Publish,
      FullTrackName{TrackNamespace{{"cat4moq.example"}}, "video"}
  ));
  EXPECT_FALSE(allows(
      grants.value(),
      Action::Publish,
      FullTrackName{TrackNamespace{{"cat4moq.example"}}, "audio"}
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
      .tokenType = 16,
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
