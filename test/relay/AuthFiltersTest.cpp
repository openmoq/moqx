/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/AuthFilters.h"

#include "auth/AuthTokenIssuer.h"

#include <folly/coro/BlockingWait.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <moxygen/test/Mocks.h>

using testing::_;
using testing::NiceMock;
using namespace moxygen;
using namespace openmoq::moqx;
using namespace openmoq::moqx::auth;

namespace {

config::AuthConfig makeVerifierConfig() {
  return config::AuthConfig{
      .enabled = true,
      .tokenType = 77,
      .hmacKeys = {config::AuthConfig::HmacKey{.id = "k1", .secret = "secret"}},
      .requireSetupToken = true,
      .allowRequestTokenOverride = true,
      .maxTokensPerMessage = 4,
  };
}

Grants makeGrants(std::vector<Action> actions) {
  Grants grants;
  grants.expiresAt = std::chrono::system_clock::now() + std::chrono::hours(1);
  grants.scopes.push_back(
      Scope{.actions = std::move(actions), .namespaceMatches = {}, .trackMatches = {}}
  );
  return grants;
}

AuthToken makeSignedToken(std::vector<Action> actions) {
  return AuthToken{
      .tokenType = 77,
      .tokenValue = signGrants("k1", "secret", makeGrants(std::move(actions))),
      .alias = AuthToken::DontRegister,
  };
}

Parameters withAuthToken(FrameType frameType, AuthToken token) {
  Parameters params(frameType);
  auto ok = params.insertParam(
      Parameter(static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN), std::move(token))
  );
  EXPECT_TRUE(ok.hasValue());
  return params;
}

class AuthFiltersTest : public ::testing::Test {
protected:
  void SetUp() override {
    verifier_ = std::make_shared<const AuthTokenVerifier>(makeVerifierConfig());
    publisherInner_ = std::make_shared<NiceMock<MockPublisher>>();
    subscriberInner_ = std::make_shared<NiceMock<MockSubscriber>>();
  }

  std::shared_ptr<AuthPublisherFilter> makePublisherFilter(std::vector<Grants> sessionGrants) {
    return std::make_shared<AuthPublisherFilter>(
        publisherInner_,
        verifier_,
        std::make_shared<const std::vector<Grants>>(std::move(sessionGrants)),
        /*peeringEnabled=*/false
    );
  }

  std::shared_ptr<AuthSubscriberFilter> makeSubscriberFilter(std::vector<Grants> sessionGrants) {
    return std::make_shared<AuthSubscriberFilter>(
        subscriberInner_,
        verifier_,
        std::make_shared<const std::vector<Grants>>(std::move(sessionGrants))
    );
  }

  std::shared_ptr<const AuthTokenVerifier> verifier_;
  std::shared_ptr<NiceMock<MockPublisher>> publisherInner_;
  std::shared_ptr<NiceMock<MockSubscriber>> subscriberInner_;
};

} // namespace

// Two session-grants entries where only the second covers Subscribe: the
// filter must permit it (any pool element satisfying is enough).
TEST_F(AuthFiltersTest, SubscribeSucceedsWhenAnySessionGrantCoversIt) {
  auto filter =
      makePublisherFilter({makeGrants({Action::Publish}), makeGrants({Action::Subscribe})});

  SubscribeOk ok;
  ok.requestID = RequestID(1);
  auto handle = std::make_shared<NiceMock<MockSubscriptionHandle>>(ok);
  EXPECT_CALL(*publisherInner_, subscribe(_, _))
      .WillOnce(
          [handle](SubscribeRequest, std::shared_ptr<TrackConsumer>)
              -> folly::coro::Task<Publisher::SubscribeResult> {
            co_return folly::makeExpected<SubscribeError>(std::shared_ptr<SubscriptionHandle>(handle
            ));
          }
      );

  SubscribeRequest sub;
  sub.requestID = RequestID(1);
  sub.fullTrackName = FullTrackName{TrackNamespace{{"live"}}, "video"};
  auto result = folly::coro::blockingWait(filter->subscribe(std::move(sub), nullptr));
  EXPECT_TRUE(result.hasValue());
}

TEST_F(AuthFiltersTest, SubscribeFailsWhenNoSessionGrantCoversIt) {
  auto filter = makePublisherFilter({makeGrants({Action::Publish})});

  SubscribeRequest sub;
  sub.requestID = RequestID(2);
  sub.fullTrackName = FullTrackName{TrackNamespace{{"live"}}, "video"};
  auto result = folly::coro::blockingWait(filter->subscribe(std::move(sub), nullptr));
  ASSERT_FALSE(result.hasValue());
  EXPECT_EQ(result.error().errorCode, SubscribeErrorCode::UNAUTHORIZED);
}

// A malformed per-request token must be dropped as a non-viable candidate,
// not block a still-valid session grant from permitting the request.
TEST_F(AuthFiltersTest, SubscribeGarbageRequestTokenDoesNotBlockSessionGrant) {
  auto filter = makePublisherFilter({makeGrants({Action::Subscribe})});

  SubscribeOk ok;
  ok.requestID = RequestID(3);
  auto handle = std::make_shared<NiceMock<MockSubscriptionHandle>>(ok);
  EXPECT_CALL(*publisherInner_, subscribe(_, _))
      .WillOnce(
          [handle](SubscribeRequest, std::shared_ptr<TrackConsumer>)
              -> folly::coro::Task<Publisher::SubscribeResult> {
            co_return folly::makeExpected<SubscribeError>(std::shared_ptr<SubscriptionHandle>(handle
            ));
          }
      );

  SubscribeRequest sub;
  sub.requestID = RequestID(3);
  sub.fullTrackName = FullTrackName{TrackNamespace{{"live"}}, "video"};
  sub.params = withAuthToken(
      FrameType::SUBSCRIBE,
      AuthToken{
          .tokenType = 77,
          .tokenValue = std::string("\xde\xad\xbe\xef", 4),
          .alias = AuthToken::DontRegister
      }
  );
  auto result = folly::coro::blockingWait(filter->subscribe(std::move(sub), nullptr));
  EXPECT_TRUE(result.hasValue());
}

TEST_F(AuthFiltersTest, PublishRejectedWhenNoSessionGrantCoversIt) {
  auto filter = makeSubscriberFilter({makeGrants({Action::Subscribe})});

  PublishRequest pub;
  pub.requestID = RequestID(4);
  pub.fullTrackName = FullTrackName{TrackNamespace{{"live"}}, "video"};
  auto result = filter->publish(std::move(pub), nullptr);
  ASSERT_FALSE(result.hasValue());
  EXPECT_EQ(result.error().errorCode, PublishErrorCode::UNAUTHORIZED);
}

// A per-request token additively unlocks Publish even though the session
// grants alone (Subscribe only) don't cover it.
TEST_F(AuthFiltersTest, PublishSucceedsWithRequestTokenWhenSessionGrantsDoNotCoverIt) {
  auto filter = makeSubscriberFilter({makeGrants({Action::Subscribe})});

  EXPECT_CALL(*subscriberInner_, publish(_, _))
      .WillOnce(
          [](PublishRequest pub, std::shared_ptr<SubscriptionHandle>) -> Subscriber::PublishResult {
            PublishOk ok;
            ok.requestID = pub.requestID;
            return Subscriber::PublishConsumerAndReplyTask{
                .consumer = nullptr,
                .reply =
                    folly::coro::makeTask<folly::Expected<PublishOk, PublishError>>(std::move(ok)),
            };
          }
      );

  PublishRequest pub;
  pub.requestID = RequestID(5);
  pub.fullTrackName = FullTrackName{TrackNamespace{{"live"}}, "video"};
  pub.params = withAuthToken(FrameType::PUBLISH, makeSignedToken({Action::Publish}));
  auto result = filter->publish(std::move(pub), nullptr);
  EXPECT_TRUE(result.hasValue());
}

// --- Anonymous claim: end-to-end through the auth filters ---

// A service-wide floor lets anyone subscribe/fetch without a token, while
// publish still requires one -- even with sessionGrants empty (as if
// require_setup_token: false and no setup token was ever presented).
TEST(
    AuthFiltersAnonymousClaimTest,
    SubscribeAndFetchAllowedAnonymouslyWhilePublishStillNeedsToken
) {
  auto config = makeVerifierConfig();
  config.anonymousClaim = {
      config::AuthConfig::AnonymousScope{.actions = {Action::Subscribe, Action::Fetch}},
  };
  auto verifier = std::make_shared<const AuthTokenVerifier>(config);
  auto publisherInner = std::make_shared<NiceMock<MockPublisher>>();
  auto subscriberInner = std::make_shared<NiceMock<MockSubscriber>>();
  auto emptySessionGrants = std::make_shared<const std::vector<Grants>>();

  AuthPublisherFilter
      pubFilter(publisherInner, verifier, emptySessionGrants, /*peeringEnabled=*/false);
  AuthSubscriberFilter subFilter(subscriberInner, verifier, emptySessionGrants);

  // Subscribe: anonymous claim covers it.
  SubscribeOk subOk;
  subOk.requestID = RequestID(1);
  auto subHandle = std::make_shared<NiceMock<MockSubscriptionHandle>>(subOk);
  EXPECT_CALL(*publisherInner, subscribe(_, _))
      .WillOnce(
          [subHandle](SubscribeRequest, std::shared_ptr<TrackConsumer>)
              -> folly::coro::Task<Publisher::SubscribeResult> {
            co_return folly::makeExpected<SubscribeError>(
                std::shared_ptr<SubscriptionHandle>(subHandle)
            );
          }
      );
  SubscribeRequest sub;
  sub.requestID = RequestID(1);
  sub.fullTrackName = FullTrackName{TrackNamespace{{"live"}}, "video"};
  EXPECT_TRUE(folly::coro::blockingWait(pubFilter.subscribe(std::move(sub), nullptr)).hasValue());

  // Fetch: anonymous claim covers it.
  EXPECT_CALL(*publisherInner, fetch(_, _))
      .WillOnce(
          [](Fetch f, std::shared_ptr<FetchConsumer>) -> folly::coro::Task<Publisher::FetchResult> {
            auto handle =
                std::make_shared<NiceMock<MockFetchHandle>>(FetchOk{.requestID = f.requestID});
            co_return folly::makeExpected<FetchError>(std::shared_ptr<Publisher::FetchHandle>(handle
            ));
          }
      );
  Fetch fetch(
      RequestID(2),
      FullTrackName{TrackNamespace{{"live"}}, "video"},
      AbsoluteLocation{0, 0},
      AbsoluteLocation{1, 0}
  );
  EXPECT_TRUE(folly::coro::blockingWait(pubFilter.fetch(std::move(fetch), nullptr)).hasValue());

  // Publish: the anonymous claim doesn't cover it -- rejected without a token.
  PublishRequest pubNoToken;
  pubNoToken.requestID = RequestID(3);
  pubNoToken.fullTrackName = FullTrackName{TrackNamespace{{"live"}}, "video"};
  auto noTokenResult = subFilter.publish(std::move(pubNoToken), nullptr);
  ASSERT_FALSE(noTokenResult.hasValue());
  EXPECT_EQ(noTokenResult.error().errorCode, PublishErrorCode::UNAUTHORIZED);

  // Publish: a per-request token additively unlocks it.
  EXPECT_CALL(*subscriberInner, publish(_, _))
      .WillOnce(
          [](PublishRequest p, std::shared_ptr<SubscriptionHandle>) -> Subscriber::PublishResult {
            PublishOk ok;
            ok.requestID = p.requestID;
            return Subscriber::PublishConsumerAndReplyTask{
                .consumer = nullptr,
                .reply =
                    folly::coro::makeTask<folly::Expected<PublishOk, PublishError>>(std::move(ok)),
            };
          }
      );
  PublishRequest pubWithToken;
  pubWithToken.requestID = RequestID(4);
  pubWithToken.fullTrackName = FullTrackName{TrackNamespace{{"live"}}, "video"};
  pubWithToken.params = withAuthToken(FrameType::PUBLISH, makeSignedToken({Action::Publish}));
  EXPECT_TRUE(subFilter.publish(std::move(pubWithToken), nullptr).hasValue());
}
