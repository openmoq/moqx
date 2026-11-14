/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "auth/AuthTokenIssuer.h"

#include "auth/Auth.h"

#include <folly/portability/GTest.h>

#include <stdexcept>
#include <tuple>

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
  EXPECT_FALSE(
      allows(grants.value(), Action::PublishNamespace, TrackNamespace{{"cat4moq.example", "event"}})
  );
  EXPECT_FALSE(allows(
      grants.value(),
      Action::Publish,
      FullTrackName{TrackNamespace{{"cat4moq.example"}}, "audio"}
  ));
  EXPECT_FALSE(allows(
      grants.value(),
      Action::Publish,
      FullTrackName{TrackNamespace{{"cat4moq.example"}}, "video-hd"}
  ));
}

TEST(AuthTokenIssuerTest, IssuesPrefixNamespaceGrants) {
  const std::string secret = "operator-test-secret-with-enough-entropy";
  IssueTokenOptions options{
      .keyID = "cat-dev",
      .secret = secret,
      .actions = {Action::ClientSetup, Action::PublishNamespace, Action::Publish},
      .trackNamespace = TrackNamespace{{"testnamespace", "cat"}},
      .namespaceMatchType = MatchRuleType::Prefix,
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
  EXPECT_TRUE(allows(
      grants.value(),
      Action::PublishNamespace,
      TrackNamespace{{"testnamespace", "cat", "event1"}}
  ));
  EXPECT_TRUE(allows(
      grants.value(),
      Action::Publish,
      FullTrackName{TrackNamespace{{"testnamespace", "cat", "event1"}}, "video"}
  ));
  EXPECT_FALSE(
      allows(grants.value(), Action::PublishNamespace, TrackNamespace{{"testnamespace", "catalog"}})
  );
  EXPECT_FALSE(allows(
      grants.value(),
      Action::Publish,
      FullTrackName{TrackNamespace{{"testnamespace", "cat", "event1"}}, "audio"}
  ));
}

TEST(AuthTokenIssuerTest, RequiresNamespaceForNonExactMatchMode) {
  IssueTokenOptions options{
      .keyID = "cat-dev",
      .secret = "operator-test-secret-with-enough-entropy",
      .actions = {Action::Publish},
      .namespaceMatchType = MatchRuleType::Prefix,
  };

  EXPECT_THROW(issueToken(options), std::invalid_argument);
}

TEST(AuthTokenIssuerTest, IssuesPrefixAndSuffixTrackGrants) {
  const std::string secret = "operator-test-secret-with-enough-entropy";
  AuthTokenVerifier verifier(config::AuthConfig{
      .enabled = true,
      .tokenType = 16,
      .hmacKeys = {config::AuthConfig::HmacKey{.id = "cat-dev", .secret = secret}},
  });
  for (const auto& [mode, allowed, denied] :
       {std::tuple{MatchRuleType::Prefix, "video-hd", "hd-video"},
        std::tuple{MatchRuleType::Suffix, "hd-video", "video-hd"}}) {
    IssueTokenOptions options{
        .keyID = "cat-dev",
        .secret = secret,
        .actions = {Action::PublishNamespace, Action::Publish},
        .trackNamespace = TrackNamespace{{"live"}},
        .namespaceMatchType = MatchRuleType::Prefix,
        .trackName = "video",
        .trackMatchType = mode,
        .now = std::chrono::system_clock::time_point(std::chrono::seconds(1'800'000'000)),
    };
    auto issued = issueToken(options);
    auto grants = verifier.verify(AuthToken{
        .tokenType = 16,
        .tokenValue = issued.tokenValue,
        .alias = AuthToken::DontRegister,
    });

    ASSERT_TRUE(grants.hasValue());
    EXPECT_TRUE(allows(grants.value(), Action::PublishNamespace, TrackNamespace{{"live", "event"}})
    );
    EXPECT_TRUE(allows(
        grants.value(),
        Action::Publish,
        FullTrackName{TrackNamespace{{"live", "event"}}, "video"}
    ));
    EXPECT_TRUE(allows(
        grants.value(),
        Action::Publish,
        FullTrackName{TrackNamespace{{"live", "event"}}, allowed}
    ));
    EXPECT_FALSE(allows(
        grants.value(),
        Action::Publish,
        FullTrackName{TrackNamespace{{"live", "event"}}, denied}
    ));
    EXPECT_FALSE(
        allows(grants.value(), Action::Publish, FullTrackName{TrackNamespace{{"other"}}, allowed})
    );
  }
}

TEST(AuthTokenIssuerTest, RequiresTrackForNonExactMatchMode) {
  IssueTokenOptions options{
      .keyID = "cat-dev",
      .secret = "operator-test-secret-with-enough-entropy",
      .actions = {Action::Publish},
  };
  for (auto mode : {MatchRuleType::Prefix, MatchRuleType::Suffix}) {
    options.trackMatchType = mode;
    EXPECT_THROW(issueToken(options), std::invalid_argument);
  }
}

TEST(AuthTokenIssuerTest, ParsesOperatorActionNames) {
  auto actions = parseActions("client_setup,publish_namespace,publish");

  ASSERT_EQ(actions.size(), 3);
  EXPECT_EQ(actions[0], Action::ClientSetup);
  EXPECT_EQ(actions[1], Action::PublishNamespace);
  EXPECT_EQ(actions[2], Action::Publish);
}

TEST(AuthTokenIssuerTest, ParsesNamespaceMatchModes) {
  EXPECT_EQ(parseNamespaceMatchType("exact"), MatchRuleType::Exact);
  EXPECT_EQ(parseNamespaceMatchType("prefix"), MatchRuleType::Prefix);
  EXPECT_EQ(parseNamespaceMatchType("suffix"), MatchRuleType::Suffix);
  EXPECT_THROW(parseNamespaceMatchType("contains"), std::invalid_argument);
  EXPECT_THROW(parseNamespaceMatchType("wildcard"), std::invalid_argument);
}

TEST(AuthTokenIssuerTest, ParsesTrackMatchModes) {
  EXPECT_EQ(parseTrackMatchType("exact"), MatchRuleType::Exact);
  EXPECT_EQ(parseTrackMatchType("prefix"), MatchRuleType::Prefix);
  EXPECT_EQ(parseTrackMatchType("suffix"), MatchRuleType::Suffix);
  EXPECT_THROW(parseTrackMatchType("contains"), std::invalid_argument);
  EXPECT_THROW(parseTrackMatchType("wildcard"), std::invalid_argument);
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
